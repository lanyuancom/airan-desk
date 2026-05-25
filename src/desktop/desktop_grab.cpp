#include "desktop_grab.h"
#include "desktop_grab_factory.h"

std::shared_ptr<DesktopGrab> DesktopGrab::createBestDesktopGrabber(int screenIndex, QObject *parent)
{
    return createDesktopGrabber(QStringLiteral("auto"), screenIndex, parent);
}

std::shared_ptr<DesktopGrab> DesktopGrab::createDesktopGrabber(const QString &backend, int screenIndex, QObject *parent)
{
    return DesktopGrabFactory::createDesktopGrabber(backend, screenIndex, parent);
}
