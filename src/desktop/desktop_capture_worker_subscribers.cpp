#include "desktop_capture_worker.h"
#include "desktop_grab.h"
#include "desktop_grab_factory.h"
#include "../codec/h264_encoder.h"

#include <QDateTime>
#include <QTimer>
#include <algorithm>

namespace
{
constexpr qint64 kStaticDesktopRefreshMs = 1000;
}

void DesktopCaptureWorker::initialize(int screenIndex, int fps)
{
    QMutexLocker locker(&m_mutex);
    m_stopping = false;

    if (m_desktopGrab)
    {
        LOG_WARN("Already initialized");
        return;
    }
    m_screenIndex = screenIndex;
    if (!ensureDesktopGrabberLocked())
    {
        return;
    }
    updateCaptureTimerLocked(fps);
}

void DesktopCaptureWorker::addSubscriber(const QString &subscriberId, int dstW, int dstH, int fps, int targetKbps, bool forceAllKeyframes)
{
    QMutexLocker locker(&m_mutex);
    m_stopping = false;

    if (!ensureDesktopGrabberLocked())
    {
        return;
    }

    if (m_subscribers.contains(subscriberId))
    {
        LOG_WARN("Subscriber {} already exists in worker", subscriberId);
        return;
    }
    auto encoder = std::make_unique<H264Encoder>(this);
    if (!encoder->initialize(m_screenIndex, dstW, dstH, fps, targetKbps, forceAllKeyframes))
    {
        LOG_ERROR("Failed to create encoder for subscriber {}", subscriberId);
        emit errorOccurred(QString("Failed to create encoder for %1").arg(subscriberId));
        return;
    }

    SubscriberInfo *info = new SubscriberInfo();
    info->id = subscriberId;
    info->dstW = dstW;
    info->dstH = dstH;
    info->fps = fps;
    info->targetKbps = targetKbps;
    info->forceAllKeyframes = forceAllKeyframes;
    info->encoder = std::move(encoder);
    info->zeroCopyActive = isZeroCopyActiveLocked(info->encoder.get());
    info->lastDirtyMs = QDateTime::currentMSecsSinceEpoch();
    emit encoderChanged(m_screenIndex,
                        info->encoder->encoderName(),
                        info->encoder->isHardwareEncoder(),
                        info->zeroCopyActive);

    m_subscribers.insert(subscriberId, info);
    LOG_INFO("Worker added subscriber {} ({}x{} @ {}fps, bitrate={}kbps, allKeyframes={})",
             subscriberId, dstW, dstH, fps, targetKbps, forceAllKeyframes);

    /* max fps 策略：只增不减可快速更新；整体仍调用一次重平衡确保一致 */
    reBalanceCaptureFps();
    scheduleNextCaptureLocked(0);
}

void DesktopCaptureWorker::removeSubscriber(const QString &subscriberId)
{
    QMutexLocker locker(&m_mutex);

    SubscriberInfo *info = m_subscribers.value(subscriberId);
    if (!info)
    {
        LOG_WARN("Subscriber {} not found in worker", subscriberId);
        return;
    }

    const int removedFps = info->fps;

    info->encoder.reset();
    delete info;
    m_subscribers.remove(subscriberId);

    LOG_INFO("Worker removed subscriber {}", subscriberId);

    if (m_subscribers.isEmpty() && m_timer && m_timer->isActive())
    {
        stopTimerLocked();
        LOG_INFO("Stopped capture timer due to no subscribers");
        if (m_desktopGrab)
        {
            m_desktopGrab->stopCapture();
            m_desktopGrab = nullptr;
            LOG_INFO("Requested desktop grabber to stop capture due to no subscribers");
        }
        return;
    }

    /* 只有删除的正好是当前采集 fps，才需要重算最大值 */
    if (removedFps >= m_captureFps)
    {
        reBalanceCaptureFps();
    }
}

void DesktopCaptureWorker::updateSubscriber(const QString &subscriberId, int dstW, int dstH, int fps, int targetKbps, bool forceAllKeyframes)
{
    QMutexLocker locker(&m_mutex);

    SubscriberInfo *info = m_subscribers.value(subscriberId);
    if (!info)
    {
        LOG_WARN("Subscriber {} not found in worker for update", subscriberId);
        return;
    }
    const bool resolutionChanged = (info->dstW != dstW || info->dstH != dstH);
    const bool rateControlChanged = (info->targetKbps != targetKbps ||
                                     info->forceAllKeyframes != forceAllKeyframes ||
                                     info->fps != fps);
    if (!resolutionChanged && !rateControlChanged)
    {
        LOG_INFO("Subscriber {} parameters unchanged, no update needed", subscriberId);
        return;
    }
    const int oldFps = info->fps;

    info->dstW = dstW;
    info->dstH = dstH;
    info->fps = fps;
    info->targetKbps = targetKbps;
    info->forceAllKeyframes = forceAllKeyframes;

    if (resolutionChanged && info->encoder)
    {
        info->encoder.reset();
        resetSubscriberGpuStateLocked(info);
        info->hasLastFrameFingerprint = false;
        info->lastFrameFingerprint = 0;
        info->unchangedFrames = 0;
        info->lastEncodedMs = 0;
        info->lastDirtyMs = QDateTime::currentMSecsSinceEpoch();
        info->forceNextFrame = true;
    }

    if (!info->encoder)
    {
        auto encoder = std::make_unique<H264Encoder>(this);
        if (!encoder->initialize(m_screenIndex, dstW, dstH, fps, targetKbps, forceAllKeyframes))
        {
            LOG_ERROR("Failed to create encoder for subscriber {}", subscriberId);
            emit errorOccurred(QString("Failed to create encoder for %1").arg(subscriberId));
            return;
        }
        info->encoder = std::move(encoder);
        info->zeroCopyActive = isZeroCopyActiveLocked(info->encoder.get());
        emit encoderChanged(m_screenIndex,
                            info->encoder->encoderName(),
                            info->encoder->isHardwareEncoder(),
                            info->zeroCopyActive);
    }
    else if (rateControlChanged && info->encoder)
    {
        if (!info->encoder->updateRateControl(fps, targetKbps, forceAllKeyframes))
        {
            LOG_WARN("In-place encoder rate-control update failed for {}, recreating encoder", subscriberId);
            info->encoder.reset();
            resetSubscriberGpuStateLocked(info);
            auto encoder = std::make_unique<H264Encoder>(this);
            if (!encoder->initialize(m_screenIndex, dstW, dstH, fps, targetKbps, forceAllKeyframes))
            {
                LOG_ERROR("Failed to recreate encoder for subscriber {}", subscriberId);
                emit errorOccurred(QString("Failed to recreate encoder for %1").arg(subscriberId));
                return;
            }
            info->encoder = std::move(encoder);
            info->zeroCopyActive = isZeroCopyActiveLocked(info->encoder.get());
            emit encoderChanged(m_screenIndex,
                                info->encoder->encoderName(),
                                info->encoder->isHardwareEncoder(),
                                info->zeroCopyActive);
        }
        info->forceNextFrame = true;
    }

    LOG_INFO("Worker updated subscriber {} to {}x{} @ {}fps, bitrate={}kbps, allKeyframes={}",
             subscriberId, dstW, dstH, fps, targetKbps, forceAllKeyframes);

    /* max fps 策略： */
    /* - fps 变大，直接提升采集 fps */
    /* - fps 变小且原来占据最大值，需重算最大值 */
    if (fps > m_captureFps)
    {
        updateCaptureTimerLocked(fps);
    }
    else if (oldFps >= m_captureFps && fps < oldFps)
    {
        reBalanceCaptureFps();
    }
}

void DesktopCaptureWorker::setCaptureBackend(const QString &backend)
{
    QMutexLocker locker(&m_mutex);
    const QString normalized = DesktopGrabFactory::normalizeBackend(backend);
    if (normalized == m_captureBackend)
    {
        /* Backend already applied. Avoid re-emitting captureBackendChanged so
         * upstream listeners do not rebroadcast redundant stream-config status
         * messages on every setCaptureBackend retry. */
        return;
    }

    LOG_INFO("Capture backend changed: {} -> {}", m_captureBackend, normalized);
    m_captureBackend = normalized;
    m_autoBackendPromotionPending = false;
    resetDesktopGrabberLocked();
    if (!m_subscribers.isEmpty())
    {
        if (!ensureDesktopGrabberLocked())
        {
            if (normalized != QStringLiteral("qt"))
            {
                LOG_WARN("Capture backend {} failed to create grabber; fallback to qt", normalized);
                m_captureBackend = QStringLiteral("qt");
                resetDesktopGrabberLocked();
                if (!ensureDesktopGrabberLocked())
                {
                    LOG_ERROR("Fallback capture backend qt also failed to create grabber");
                    return;
                }
            }
            else
            {
                LOG_ERROR("Capture backend qt failed to create grabber");
                return;
            }
        }
        recreateSubscriberEncodersLocked();
        scheduleNextCaptureLocked(0);
    }
}

void DesktopCaptureWorker::requestKeyframe(const QString &subscriberId)
{
    QMutexLocker locker(&m_mutex);
    SubscriberInfo *subscriber = m_subscribers.value(subscriberId, nullptr);
    if (!subscriber || !subscriber->encoder)
    {
        LOG_WARN("Keyframe request ignored: subscriber {} not found", subscriberId);
        return;
    }

    subscriber->encoder->requestKeyframe();
    subscriber->forceNextFrame = true;
    LOG_INFO("Keyframe requested for subscriber {}", subscriberId);
}

void DesktopCaptureWorker::stopCapture()
{
    QMutexLocker locker(&m_mutex);

    if (m_timer && m_timer->isActive())
    {
        stopTimerLocked();
    }
    m_stopping = true;
    m_captureInProgress = false;

    if (m_desktopGrab)
    {
        m_desktopGrab->stopCapture();
        m_desktopGrab = nullptr;
    }

    for (auto info : m_subscribers)
    {
        if (info)
        {
            info->encoder.reset();
            delete info;
        }
    }
    m_subscribers.clear();
}

void DesktopCaptureWorker::reBalanceCaptureFps()
{
    /* 调用方必须已持有 m_mutex（当前仅在 add/update/remove 内调用） */
    if (m_subscribers.empty())
    {
        LOG_WARN("No subscribers to rebalance fps");
        if (m_timer && m_timer->isActive())
        {
            stopTimerLocked();
            LOG_INFO("Stopped capture timer due to no subscribers");
        }
        return;
    }
    int newMaxFps = 0;
    for (auto it : m_subscribers.values())
    {
        if (it && it->fps > newMaxFps)
        {
            newMaxFps = it->fps;
        }
    }

    updateCaptureTimerLocked(newMaxFps);
}

void DesktopCaptureWorker::deleteInThread()
{
    delete this;
}

void DesktopCaptureWorker::updateCaptureTimerLocked(int fps)
{
    if (fps <= 0)
    {
        fps = 30;
    }

    if (!m_timer)
    {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, &DesktopCaptureWorker::onTimeout);
    }

    if (fps == m_captureFps)
    {
        scheduleNextCaptureLocked();
        return;
    }

    LOG_INFO("Capture fps updated: {} -> {}", m_captureFps, fps);
    m_captureFps = fps;
    scheduleNextCaptureLocked();
}

void DesktopCaptureWorker::scheduleNextCaptureLocked(int delayMs)
{
    if (m_stopping || m_subscribers.isEmpty())
    {
        m_nextCaptureDueMs = 0;
        return;
    }

    if (!m_timer)
    {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, &DesktopCaptureWorker::onTimeout);
    }

    if (m_captureInProgress)
    {
        return;
    }

    const int intervalMs = std::max(1, 1000 / std::max(1, m_captureFps));
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    int nextDelayMs = delayMs;
    if (nextDelayMs < 0)
    {
        if (m_nextCaptureDueMs <= 0 || nowMs - m_nextCaptureDueMs > intervalMs * 2)
        {
            m_nextCaptureDueMs = nowMs + intervalMs;
        }
        nextDelayMs = static_cast<int>(std::max<qint64>(1, m_nextCaptureDueMs - nowMs));
    }
    else if (nextDelayMs == 0)
    {
        m_nextCaptureDueMs = nowMs;
    }
    else
    {
        m_nextCaptureDueMs = nowMs + nextDelayMs;
    }
    if (m_timer->isActive())
    {
        m_timer->stop();
    }
    m_timer->start(nextDelayMs);
}

void DesktopCaptureWorker::stopTimerLocked()
{
    if (m_timer && m_timer->isActive())
    {
        m_timer->stop();
    }
    m_nextCaptureDueMs = 0;
}

bool DesktopCaptureWorker::anySubscriberNeedsStaticCpuRefreshLocked(qint64 nowMs) const
{
    for (auto subscriber : m_subscribers)
    {
        if (!subscriber || !subscriber->encoder)
            continue;
        if (subscriber->forceNextFrame || subscriber->lastEncodedMs <= 0 ||
            nowMs - subscriber->lastEncodedMs >= kStaticDesktopRefreshMs)
        {
            return true;
        }
    }
    return false;
}
