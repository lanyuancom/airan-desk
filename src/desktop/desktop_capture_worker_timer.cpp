#include "desktop_capture_worker.h"

#include <QDateTime>
#include <algorithm>

void DesktopCaptureWorker::onTimeout()
{
    const qint64 captureStartMs = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker locker(&m_mutex);
        if (m_stopping || m_subscribers.isEmpty())
        {
            m_captureInProgress = false;
            stopTimerLocked();
            return;
        }
        if (m_captureInProgress)
        {
            LOG_TRACE("Skip capture tick because previous capture is still in progress");
            return;
        }
        m_captureInProgress = true;
        m_captureCostTimer.restart();
    }

    bool ok = false;
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    if (captureAndEncodeGPU())
    {
        ok = true;
    }
    else
#endif
    {
        ok = captureAndEncodeCPU();
    }

    QMutexLocker locker(&m_mutex);
    const qint64 costMs = m_captureCostTimer.isValid() ? m_captureCostTimer.elapsed() : 0;
    m_captureInProgress = false;
    if (!ok)
    {
        LOG_TRACE("Capture tick finished without encoded frame, cost={}ms", costMs);
    }
    const int intervalMs = std::max(1, 1000 / std::max(1, m_captureFps));
    const qint64 nextDueMs = captureStartMs + intervalMs;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const int delayMs = static_cast<int>(std::max<qint64>(1, nextDueMs - nowMs));
    m_nextCaptureDueMs = nextDueMs;
    if (costMs > intervalMs)
    {
        LOG_TRACE("Capture/encode cost {}ms exceeds target interval {}ms, next frame will be scheduled after current frame completion", costMs, intervalMs);
    }
    scheduleNextCaptureLocked(delayMs);
}
