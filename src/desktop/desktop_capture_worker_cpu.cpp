#include "desktop_capture_worker.h"
#include "desktop_grab.h"
#include "../codec/h264_encoder.h"

#include <QDateTime>
#include <algorithm>

namespace
{
constexpr qint64 kStaticDesktopRefreshMs = 1000;

quint64 sampledFrameFingerprint(const QImage &image)
{
    if (image.isNull())
        return 0;

    QImage view = image;
    if (view.format() != QImage::Format_ARGB32 && view.format() != QImage::Format_RGB32)
        view = image.convertToFormat(QImage::Format_ARGB32);

    quint64 hash = 1469598103934665603ULL;
    auto mix = [&hash](quint64 value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };

    mix(static_cast<quint64>(view.width()));
    mix(static_cast<quint64>(view.height()));
    mix(static_cast<quint64>(view.bytesPerLine()));

    const int stepY = std::max(1, view.height() / 96);
    const int stepX = std::max(1, view.width() / 128);
    for (int y = 0; y < view.height(); y += stepY)
    {
        const quint32 *row = reinterpret_cast<const quint32 *>(view.constScanLine(y));
        for (int x = 0; x < view.width(); x += stepX)
            mix(static_cast<quint64>(row[x]) + static_cast<quint64>((y << 16) ^ x));
    }

    return hash;
}
}

bool DesktopCaptureWorker::captureAndEncodeCPU()
{
    QMutexLocker locker(&m_mutex);
    QImage frame;
    if (!m_desktopGrab || !m_desktopGrab->grabFrameCPU(frame))
    {
        LOG_WARN("Failed to grab CPU frame");
        return false;
    }
    /* 遍历所有订阅者，使用各自的编码器编码 */
    /* 注意：编码器初始化使用的是 dstW/dstH，CPU 路径必须输出完全一致的分辨率。 */
    /* 若使用 KeepAspectRatio 会得到 1305x816 这类尺寸，触发 encodeCPU size mismatch。 */
    const QImage sourceFrame = frame;
    const quint64 fingerprint = sampledFrameFingerprint(sourceFrame);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (auto it : m_subscribers.values())
    {
        if (it->encoder)
        {
            const bool dirty = !it->hasLastFrameFingerprint || it->lastFrameFingerprint != fingerprint;
            const bool staticRefreshDue = it->lastEncodedMs <= 0 || nowMs - it->lastEncodedMs >= kStaticDesktopRefreshMs;
            if (!dirty && !it->forceNextFrame && !staticRefreshDue)
            {
                ++it->unchangedFrames;
                LOG_TRACE("Skip unchanged CPU frame for subscriber {} (unchangedFrames={})", it->id, it->unchangedFrames);
                continue;
            }
            if (dirty)
            {
                it->lastFrameFingerprint = fingerprint;
                it->hasLastFrameFingerprint = true;
                it->unchangedFrames = 0;
                it->lastDirtyMs = nowMs;
            }

            const QSize targetSize(it->dstW, it->dstH);
            const bool needsScale = sourceFrame.size() != targetSize;
            const QImage scaledFrame = needsScale
                                           ? sourceFrame.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                           : sourceFrame;
            auto result = it->encoder->encodeCPU(scaledFrame);
            if (!result.first->empty())
            {
                emit frameEncoded(it->id, result.first, result.second);
                it->lastEncodedMs = nowMs;
                it->forceNextFrame = false;

                LOG_TRACE("Encoded CPU frame for subscriber {} ({}x{} @ {}fps) in worker thread",
                          it->id, it->dstW, it->dstH, it->fps);
            }
        }
    }
    return true;
}
