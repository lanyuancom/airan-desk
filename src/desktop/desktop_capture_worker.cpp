#include "desktop_capture_worker.h"
#include "desktop_grab.h"
#include "desktop_grab_factory.h"
#include "../codec/h264_encoder.h"

DesktopCaptureWorker::DesktopCaptureWorker(QObject *parent)
    : QObject(parent), m_timer(nullptr), m_desktopGrab(nullptr)
{
    LOG_TRACE("Created DesktopCaptureWorker this={} in thread {}", (void *)this, (void *)QThread::currentThread());
}

DesktopCaptureWorker::~DesktopCaptureWorker()
{
    disconnect();
    if (m_timer)
    {
        if (m_timer->thread() == QThread::currentThread())
        {
            m_timer->stop();
            delete m_timer;
        }
        else
        {
            LOG_WARN("DesktopCaptureWorker destroyed from non-owner thread, defer timer deletion to timer thread");
            QMetaObject::invokeMethod(m_timer, "stop", Qt::QueuedConnection);
            m_timer->deleteLater();
        }
        m_timer = nullptr;
    }

    if (m_desktopGrab)
    {
        disconnect(m_desktopGrab.get(), nullptr, this, nullptr);
        m_desktopGrab = nullptr;
    }

    for (auto info : m_subscribers)
    {
        info->encoder.reset();
        delete info;
    }
    m_subscribers.clear();

    LOG_INFO("Destroyed");
}

bool DesktopCaptureWorker::ensureDesktopGrabberLocked()
{
    if (m_desktopGrab)
    {
        return true;
    }

    m_desktopGrab = DesktopGrab::createDesktopGrabber(m_captureBackend, m_screenIndex, nullptr);
    if (!m_desktopGrab)
    {
        LOG_ERROR("Failed to create desktop grabber");
        emit errorOccurred("Failed to create desktop grabber");
        return false;
    }
    m_actualCaptureBackend = DesktopGrabFactory::lastCreatedBackend();
    LOG_INFO("Desktop capture grabber ready: requested={}, actual={}",
             m_captureBackend,
             m_actualCaptureBackend.isEmpty() ? QStringLiteral("unknown") : m_actualCaptureBackend);
    emit captureBackendChanged(m_screenIndex, m_captureBackend, m_actualCaptureBackend);
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    if (m_captureBackend == QStringLiteral("auto") &&
        m_actualCaptureBackend == QStringLiteral("qt") &&
        !m_autoBackendPromotionPending)
    {
        m_autoBackendPromotionPending = true;
        QTimer::singleShot(800, this, &DesktopCaptureWorker::retryAutoBackendPromotion);
    }
#endif
    return true;
}
