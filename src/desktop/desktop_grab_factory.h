#ifndef DESKTOP_GRAB_FACTORY_H
#define DESKTOP_GRAB_FACTORY_H

#include <QString>
#include <QStringList>
#include <QObject>
#include <memory>

class DesktopGrab;

namespace DesktopGrabFactory
{
    QString normalizeBackend(const QString &backend);
    QStringList availableBackends();
    QString lastCreatedBackend();
    /**
     * Returns a recommended fps cap for the current platform/backend.
     * Returns -1 when there is no platform cap (modern path: WGC, PipeWire).
     * Returns a small positive value (e.g. 15) on legacy paths where the
     * Qt screen-grab fallback is the only option (Win7/8) and high fps would
     * starve the CPU.
     */
    int recommendedMaxFps();
    std::shared_ptr<DesktopGrab> createDesktopGrabber(const QString &backend, int screenIndex, QObject *parent = nullptr);
}

#endif /* DESKTOP_GRAB_FACTORY_H */
