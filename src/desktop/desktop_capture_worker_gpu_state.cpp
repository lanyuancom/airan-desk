/* Split from desktop_capture_worker_gpu.cpp by GPU capture responsibility. */

#include "desktop_capture_worker.h"
#include "desktop_grab.h"
#include "../codec/h264_encoder.h"

void DesktopCaptureWorker::retryAutoBackendPromotion()
{
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    QMutexLocker locker(&m_mutex);
    m_autoBackendPromotionPending = false;

    if (m_captureBackend != QStringLiteral("auto") ||
        m_actualCaptureBackend != QStringLiteral("qt") ||
        m_subscribers.isEmpty())
    {
        return;
    }

    LOG_INFO("Retrying WGC for auto capture backend after initial Qt fallback");
    m_captureBackend = QStringLiteral("wgc");
    resetDesktopGrabberLocked();
    if (!ensureDesktopGrabberLocked())
    {
        LOG_WARN("Auto backend WGC retry failed; fallback to qt");
        m_captureBackend = QStringLiteral("qt");
        resetDesktopGrabberLocked();
        if (!ensureDesktopGrabberLocked())
        {
            LOG_ERROR("Auto backend fallback qt failed after WGC retry");
            return;
        }
    }

    recreateSubscriberEncodersLocked();
    scheduleNextCaptureLocked(0);
#endif
}

void DesktopCaptureWorker::resetDesktopGrabberLocked()
{
    if (m_desktopGrab)
    {
        m_desktopGrab->stopCapture();
        m_desktopGrab = nullptr;
    }
    m_actualCaptureBackend.clear();
}

void DesktopCaptureWorker::resetSubscriberGpuStateLocked(SubscriberInfo *subscriber)
{
    if (!subscriber)
        return;

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    subscriber->scaleDevice.Reset();
    subscriber->scaleVideoDevice.Reset();
    subscriber->scaleVideoContext.Reset();
    subscriber->scaleVpEnum.Reset();
    subscriber->scaleProcessor.Reset();
    subscriber->scaleInputTex.Reset();
    subscriber->scaleInputView.Reset();
    subscriber->scaleOutputTex.Reset();
    subscriber->scaleOutputView.Reset();
    subscriber->scaleSrcW = 0;
    subscriber->scaleSrcH = 0;
    subscriber->scaleSrcFormat = DXGI_FORMAT_UNKNOWN;
#endif
}

bool DesktopCaptureWorker::recreateSubscriberEncoderLocked(SubscriberInfo *subscriber)
{
    if (!subscriber)
        return false;

    subscriber->encoder.reset();
    resetSubscriberGpuStateLocked(subscriber);

    auto encoder = std::make_unique<H264Encoder>(this);
    if (!encoder->initialize(m_screenIndex, subscriber->dstW, subscriber->dstH, subscriber->fps,
                             subscriber->targetKbps, subscriber->forceAllKeyframes))
    {
        LOG_ERROR("Failed to recreate encoder for subscriber {}", subscriber->id);
        emit errorOccurred(QString("Failed to recreate encoder for %1").arg(subscriber->id));
        return false;
    }

    subscriber->encoder = std::move(encoder);
    subscriber->zeroCopyActive = isZeroCopyActiveLocked(subscriber->encoder.get());
    emit encoderChanged(m_screenIndex,
                        subscriber->encoder->encoderName(),
                        subscriber->encoder->isHardwareEncoder(),
                        subscriber->zeroCopyActive);
    LOG_INFO("Recreated encoder for subscriber {} after capture backend change", subscriber->id);
    return true;
}

bool DesktopCaptureWorker::recreateSubscriberEncodersLocked()
{
    bool ok = true;
    for (auto info : m_subscribers.values())
    {
        ok = recreateSubscriberEncoderLocked(info) && ok;
    }
    return ok;
}

bool DesktopCaptureWorker::isZeroCopyActiveLocked(const H264Encoder *encoder) const
{
    return encoder
           && m_actualCaptureBackend == QStringLiteral("wgc")
           && encoder->isZeroCopyActive();
}
