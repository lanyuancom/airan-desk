#include "desktop_grab_qt.h"
#include "../common/constant.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPixmap>
#include <QScreen>
#include <QThread>

DesktopGrabQt::DesktopGrabQt(QObject *parent)
    : DesktopGrab(parent)
{
    QThread *guiThread = QCoreApplication::instance() ? QCoreApplication::instance()->thread() : nullptr;
    if (!parent && guiThread && thread() != guiThread)
    {
        moveToThread(guiThread);
    }
}

DesktopGrabQt::~DesktopGrabQt()
{
}

bool DesktopGrabQt::init(int screenIndex)
{
    if (thread() == QThread::currentThread())
    {
        return initOnGuiThread(screenIndex);
    }

    bool ok = false;
    if (!QMetaObject::invokeMethod(this, "initOnGuiThread", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(bool, ok),
                                  Q_ARG(int, screenIndex)))
    {
        LOG_ERROR("Failed to invoke DesktopGrabQt init on GUI thread");
        return false;
    }
    return ok;
}

bool DesktopGrabQt::initOnGuiThread(int screenIndex)
{
    QMutexLocker locker(&m_mutex);

    QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.isEmpty())
    {
        LOG_ERROR("No screens found");
        return false;
    }

    if (screenIndex < 0)
    {
        LOG_WARN("Invalid screen index, using primary screen (0) instead");
        screenIndex = 0;
    }
    else if (screenIndex >= screens.size())
    {
        LOG_WARN("Screen index {} out of range, using primary screen (0) instead, available screens={}",
                 screenIndex, screens.size());
        screenIndex = 0;
    }

    m_screenIndex = screenIndex;
    m_screen = screens[m_screenIndex];

    const QRect screenGeometry = m_screen ? m_screen->geometry() : QRect(0, 0, 1920, 1080);
    const qreal dpr = m_screen ? m_screen->devicePixelRatio() : 1.0;
    m_screen_width = static_cast<int>(screenGeometry.width() * dpr);
    m_screen_height = static_cast<int>(screenGeometry.height() * dpr);

    LOG_INFO("DesktopGrabQt initialized: screenIndex={}, logicalSize={}x{}, devicePixelRatio={}, physicalSize={}x{}",
             m_screenIndex,
             screenGeometry.width(), screenGeometry.height(),
             dpr,
             m_screen_width, m_screen_height);

    return true;
}

bool DesktopGrabQt::grabFrameCPU(QImage &outImage)
{
    QImage image;
    if (thread() == QThread::currentThread())
    {
        image = grabFrameOnGuiThread();
    }
    else if (!QMetaObject::invokeMethod(this, "grabFrameOnGuiThread", Qt::BlockingQueuedConnection,
                                       Q_RETURN_ARG(QImage, image)))
    {
        LOG_ERROR("Failed to invoke DesktopGrabQt grabFrame on GUI thread");
        return false;
    }

    if (image.isNull())
    {
        return false;
    }

    outImage = image;
    return true;
}

QImage DesktopGrabQt::grabFrameOnGuiThread()
{
    QMutexLocker locker(&m_mutex);
    if (!m_screen)
    {
        LOG_ERROR("Screen not initialized");
        return QImage();
    }

    QPixmap pixmap = m_screen->grabWindow(0);
    if (pixmap.isNull())
    {
        LOG_WARN("Qt screen grab returned empty pixmap, please check screen capture permission or current display state");
        return QImage();
    }

    return pixmap.toImage();
}
