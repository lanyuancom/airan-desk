#include "desktop_capture_manager.h"
#include "desktop_capture_worker.h"
#include "desktop_grab_factory.h"
#include <QGuiApplication>
#include <QScreen>

DesktopCaptureManager *DesktopCaptureManager::instance()
{
    static DesktopCaptureManager s_instance(nullptr);
    return &s_instance;
}

DesktopCaptureManager::DesktopCaptureManager(QObject *parent) : QObject(parent)
{
}

DesktopCaptureManager::~DesktopCaptureManager()
{
    for (auto it = m_workers.begin(); it != m_workers.end(); ++it)
    {
        const int screenIndex = it.key();
        DesktopCaptureWorker *worker = it.value();
        if (!worker)
        {
            continue;
        }

        QThread *workerThread = m_workerThreads.value(screenIndex, nullptr);
        if (worker->thread() == QThread::currentThread())
        {
            worker->stopCapture();
        }
        else if (workerThread && workerThread->isRunning())
        {
            /* stopCapture 内会操作 QTimer，必须在 worker 所在线程执行 */
            QMetaObject::invokeMethod(worker, "stopCapture", Qt::BlockingQueuedConnection);
        }
        else
        {
            worker->stopCapture();
        }

        if (workerThread && workerThread->isRunning() && worker->thread() == workerThread)
        {
            /* 在 worker 所在线程中同步销毁 QObject，避免 QTimer/线程亲和性告警。 */
            /* 不能用 functor 版本 invokeMethod：armhf 上的旧 Qt 头文件可能不支持该重载。 */
            worker->disconnect();
            QMetaObject::invokeMethod(worker, "deleteInThread", Qt::BlockingQueuedConnection);
        }
        else
        {
            worker->disconnect();
            delete worker;
        }

        it.value() = nullptr;
    }

    for (QThread *workerThread : m_workerThreads)
    {
        STOP_PTR_THREAD(workerThread);
        delete workerThread;
    }

    m_workers.clear();
    m_workerThreads.clear();
}

bool DesktopCaptureManager::subscribe(const QString &subscriberId, int screenIndex, int dstW, int dstH, int fps,
                                      int targetKbps, bool forceAllKeyframes)
{
    QMutexLocker locker(&m_mutex);

    if (subscriberId.isEmpty())
    {
        LOG_ERROR("subscribe failed: empty subscriberId");
        return false;
    }
    if (m_workerThreads.contains(screenIndex) == false)
    {
        /* 创建工作线程 */
        QThread *workerThread = new QThread();
        workerThread->setObjectName(QString("DesktopCaptureWorkerThread-Screen%1").arg(screenIndex));
        m_workerThreads.insert(screenIndex, workerThread);
        workerThread->start();
        LOG_INFO("Created worker thread for screenIndex {}", screenIndex);
    }
    if (!m_workers.contains(screenIndex))
    {
        /* 创建工作对象 */
        DesktopCaptureWorker *worker = new DesktopCaptureWorker();
        m_workers.insert(screenIndex, worker);
        /* 连接信号 */
        connect(worker, &DesktopCaptureWorker::frameEncoded,
                this, &DesktopCaptureManager::onWorkerFrameEncoded, Qt::QueuedConnection);

        connect(worker, &DesktopCaptureWorker::errorOccurred,
                this, &DesktopCaptureManager::onWorkerError, Qt::QueuedConnection);
        connect(worker, &DesktopCaptureWorker::captureBackendChanged,
                this, &DesktopCaptureManager::onWorkerCaptureBackendChanged, Qt::QueuedConnection);
        connect(worker, &DesktopCaptureWorker::encoderChanged,
                this, &DesktopCaptureManager::onWorkerEncoderChanged, Qt::QueuedConnection);
        /* 将工作对象移到工作线程 */
        worker->moveToThread(m_workerThreads.value(screenIndex));
        const QString requestedBackend = m_requestedBackends.value(screenIndex, QStringLiteral("auto"));
        if (!requestedBackend.isEmpty() && requestedBackend != QStringLiteral("auto"))
        {
            QMetaObject::invokeMethod(worker, "setCaptureBackend", Qt::QueuedConnection,
                                      Q_ARG(QString, requestedBackend));
        }
        /* 启动线程 */
        LOG_INFO("DesktopCaptureManager initialized, worker thread started for screenIndex {}", screenIndex);
        QMetaObject::invokeMethod(worker, "initialize", Qt::QueuedConnection,
                                  Q_ARG(int, screenIndex),
                                  Q_ARG(int, fps));
    }

    const bool already = m_subscriberScreens.contains(subscriberId);
    bool movedScreen = false;

    if (!already)
    {
        m_subscriberScreens.insert(subscriberId, screenIndex);
    }
    else if (m_subscriberScreens.value(subscriberId) != screenIndex)
    {
        const int oldScreenIndex = m_subscriberScreens.value(subscriberId);
        DesktopCaptureWorker *oldWorker = m_workers.value(oldScreenIndex, nullptr);
        if (oldWorker)
        {
            QMetaObject::invokeMethod(oldWorker, "removeSubscriber", Qt::QueuedConnection,
                                      Q_ARG(QString, subscriberId));
        }
        m_subscriberScreens[subscriberId] = screenIndex;
        movedScreen = true;
    }

    LOG_INFO("Subscriber {} subscribe ({}x{} @ {}fps, bitrate={}kbps, allKeyframes={}), totalSubscribers={}, already={}",
             subscriberId, dstW, dstH, fps, targetKbps, forceAllKeyframes, m_subscriberScreens.size(), already);

    if (already && !movedScreen && m_workers.value(screenIndex))
    {
        /* 同一个控制端重连 id 不变：这里应该更新参数，而不是重复 add */
        QMetaObject::invokeMethod(m_workers.value(screenIndex), "updateSubscriber", Qt::QueuedConnection,
                                  Q_ARG(QString, subscriberId),
                                  Q_ARG(int, dstW),
                                  Q_ARG(int, dstH),
                                  Q_ARG(int, fps),
                                  Q_ARG(int, targetKbps),
                                  Q_ARG(bool, forceAllKeyframes));
    }
    else
    {
        QMetaObject::invokeMethod(m_workers.value(screenIndex), "addSubscriber", Qt::QueuedConnection,
                                  Q_ARG(QString, subscriberId),
                                  Q_ARG(int, dstW),
                                  Q_ARG(int, dstH),
                                  Q_ARG(int, fps),
                                  Q_ARG(int, targetKbps),
                                  Q_ARG(bool, forceAllKeyframes));
    }

    return true;
}

bool DesktopCaptureManager::setCaptureBackend(const QString &backend, int screenIndex)
{
    QMutexLocker locker(&m_mutex);
    const QString normalized = DesktopGrabFactory::normalizeBackend(backend);
    if (!DesktopGrabFactory::availableBackends().contains(normalized))
    {
        LOG_WARN("Capture backend {} is not available on controlled side", normalized);
        return false;
    }
    m_requestedBackends[screenIndex] = normalized;

    DesktopCaptureWorker *worker = m_workers.value(screenIndex, nullptr);
    if (!worker)
    {
        LOG_INFO("Capture backend {} stored before worker exists", normalized);
        return true;
    }

    QMetaObject::invokeMethod(worker, "setCaptureBackend", Qt::QueuedConnection,
                              Q_ARG(QString, normalized));
    return true;
}

bool DesktopCaptureManager::requestKeyframe(const QString &subscriberId, int screenIndex)
{
    QMutexLocker locker(&m_mutex);
    DesktopCaptureWorker *worker = m_workers.value(screenIndex, nullptr);
    if (!worker)
    {
        LOG_WARN("Keyframe request ignored: worker for screenIndex {} not found", screenIndex);
        return false;
    }

    QMetaObject::invokeMethod(worker, "requestKeyframe", Qt::QueuedConnection,
                              Q_ARG(QString, subscriberId));
    return true;
}

QStringList DesktopCaptureManager::availableCaptureBackends() const
{
    return DesktopGrabFactory::availableBackends();
}

void DesktopCaptureManager::unsubscribe(const QString &subscriberId, int screenIndex)
{
    QMutexLocker locker(&m_mutex);

    if (!m_subscriberScreens.contains(subscriberId))
    {
        LOG_WARN("unsubscribe ignored: subscriber {} not found (totalSubscribers={})", subscriberId, m_subscriberScreens.size());
        return;
    }

    const int actualScreenIndex = m_subscriberScreens.value(subscriberId, screenIndex);
    m_subscriberScreens.remove(subscriberId);

    LOG_INFO("Subscriber {} unsubscribe, totalSubscribers={}", subscriberId, m_subscriberScreens.size());

    DesktopCaptureWorker *worker = m_workers.value(actualScreenIndex, nullptr);
    if (!worker)
    {
        LOG_WARN("unsubscribe ignored: worker for screenIndex {} not found", actualScreenIndex);
        return;
    }

    QMetaObject::invokeMethod(worker, "removeSubscriber", Qt::QueuedConnection,
                              Q_ARG(QString, subscriberId));
}

int DesktopCaptureManager::subscriberCount()
{
    QMutexLocker locker(&m_mutex);
    return m_subscriberScreens.size();
}

void DesktopCaptureManager::onWorkerFrameEncoded(const QString &subscriberId, std::shared_ptr<rtc::binary> encodedData, quint64 timestamp_us)
{
    LOG_TRACE("DesktopCaptureManager::onWorkerFrameEncoded invoked for {} size={}", subscriberId,
              encodedData ? (int)encodedData->size() : -1);
    /* 直接转发信号（从工作线程到主线程） */
    emit frameEncoded(subscriberId, encodedData, timestamp_us);
}

void DesktopCaptureManager::onWorkerError(const QString &errorMessage)
{
    LOG_ERROR("Worker error: {}", errorMessage);
    emit captureError(errorMessage);
}

void DesktopCaptureManager::onWorkerCaptureBackendChanged(int screenIndex, const QString &requestedBackend, const QString &actualBackend)
{
    {
        QMutexLocker locker(&m_mutex);
        if (!actualBackend.isEmpty())
        {
            m_requestedBackends[screenIndex] = actualBackend;
        }
    }

    LOG_INFO("Worker capture backend changed: screenIndex={}, requested={}, actual={}",
             screenIndex, requestedBackend, actualBackend);
    emit captureBackendChanged(screenIndex, requestedBackend, actualBackend);
}

void DesktopCaptureManager::onWorkerEncoderChanged(int screenIndex, const QString &encoderName, bool isHardware, bool zeroCopy)
{
    LOG_INFO("Worker encoder changed: screenIndex={}, encoder={}, hardware={}, zeroCopy={}",
             screenIndex, encoderName, isHardware, zeroCopy);
    emit encoderChanged(screenIndex, encoderName, isHardware, zeroCopy);
}
