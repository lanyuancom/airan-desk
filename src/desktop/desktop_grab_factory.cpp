#include "desktop_grab_factory.h"
#include "desktop_capture_plugin.h"
#include "desktop_grab.h"
#include "desktop_grab_qt.h"
#include "../common/constant.h"

#ifdef AIRAN_HAS_PIPEWIRE
#include "desktop_grab_pipewire.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QLibrary>
#include <QMap>
#include <QMutex>
#include <QSet>

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
    QString g_lastCreatedBackend;

    struct PluginRuntime
    {
        QLibrary library;
        airanCreateDesktopGrabberFn create{nullptr};
        airanDestroyDesktopGrabberFn destroy{nullptr};
        airanIsDesktopGrabberSupportedFn supported{nullptr};
        bool attempted{false};
    };

    class PluginGrabberDeleter
    {
    public:
        explicit PluginGrabberDeleter(PluginRuntime *runtime)
            : m_runtime(runtime)
        {
        }

        void operator()(DesktopGrab *grab) const
        {
            if (grab && m_runtime)
                m_runtime->destroy(grab);
        }

    private:
        PluginRuntime *m_runtime = nullptr;
    };

    QString pluginFileName(const QString &backend)
    {
#if defined(Q_OS_WIN)
        return QStringLiteral("airan_capture_%1.dll").arg(backend);
#elif defined(Q_OS_MACOS)
        return QStringLiteral("libairan_capture_%1.dylib").arg(backend);
#else
        return QStringLiteral("libairan_capture_%1.so").arg(backend);
#endif
    }

    PluginRuntime &pluginRuntime(const QString &backend)
    {
        static QMutex mutex;
        static QMap<QString, PluginRuntime *> plugins;

        QMutexLocker locker(&mutex);
        PluginRuntime *runtime = plugins.value(backend, nullptr);
        if (!runtime)
        {
            runtime = new PluginRuntime();
            plugins.insert(backend, runtime);
        }
        return *runtime;
    }

    bool loadPlugin(const QString &backend)
    {
        PluginRuntime &runtime = pluginRuntime(backend);
        if (runtime.attempted)
        {
            return runtime.library.isLoaded() && runtime.create && runtime.destroy;
        }

        runtime.attempted = true;
        const QString path = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(pluginFileName(backend));
        runtime.library.setFileName(path);
        if (!runtime.library.load())
        {
            LOG_INFO("Desktop capture plugin {} unavailable: {}", path, runtime.library.errorString());
            return false;
        }

        runtime.create = reinterpret_cast<airanCreateDesktopGrabberFn>(runtime.library.resolve(AIRAN_CAPTURE_PLUGIN_CREATE));
        runtime.destroy = reinterpret_cast<airanDestroyDesktopGrabberFn>(runtime.library.resolve(AIRAN_CAPTURE_PLUGIN_DESTROY));
        runtime.supported = reinterpret_cast<airanIsDesktopGrabberSupportedFn>(runtime.library.resolve(AIRAN_CAPTURE_PLUGIN_SUPPORTED));

        if (!runtime.create || !runtime.destroy)
        {
            LOG_WARN("Desktop capture plugin {} missing required exports", path);
            runtime.library.unload();
            return false;
        }

        LOG_INFO("Loaded desktop capture plugin {}", path);
        return true;
    }

    bool pluginSupported(const QString &backend)
    {
        if (!loadPlugin(backend))
        {
            return false;
        }

        PluginRuntime &runtime = pluginRuntime(backend);
        return !runtime.supported || runtime.supported();
    }

    std::shared_ptr<DesktopGrab> createPluginGrabber(const QString &backend, int screenIndex)
    {
        if (!pluginSupported(backend))
        {
            return nullptr;
        }

        PluginRuntime &runtime = pluginRuntime(backend);
        DesktopGrab *raw = runtime.create ? runtime.create() : nullptr;
        if (!raw)
        {
            return nullptr;
        }

        std::shared_ptr<DesktopGrab> grabber(raw, PluginGrabberDeleter(&runtime));

        if (!grabber->init(screenIndex))
        {
            LOG_WARN("Desktop capture plugin backend {} failed to initialize", backend);
            return nullptr;
        }
        return grabber;
    }
}

QString DesktopGrabFactory::normalizeBackend(const QString &backend)
{
    const QString value = backend.trimmed().toLower();
    if (value == QStringLiteral("wgc") ||
        value == QStringLiteral("qt") ||
        value == QStringLiteral("pipewire"))
    {
        return value;
    }
    return QStringLiteral("auto");
}

QStringList DesktopGrabFactory::availableBackends()
{
    QStringList result;
    result << QStringLiteral("auto") << QStringLiteral("qt");
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    if (pluginSupported(QStringLiteral("wgc")))
    {
        result << QStringLiteral("wgc");
    }
#endif
#ifdef AIRAN_HAS_PIPEWIRE
    if (DesktopGrabPipeWire::isAvailable())
    {
        result << QStringLiteral("pipewire");
    }
#endif
    return result;
}

QString DesktopGrabFactory::lastCreatedBackend()
{
    return g_lastCreatedBackend;
}

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
namespace
{
    struct WinVer { DWORD major{0}; DWORD minor{0}; DWORD build{0}; };

    WinVer queryWinVersion()
    {
        WinVer v{};
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll)
            return v;
        using RtlGetVersionFn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (!fn)
            return v;
        RTL_OSVERSIONINFOW osvi{};
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        if (fn(&osvi) == 0)
        {
            v.major = osvi.dwMajorVersion;
            v.minor = osvi.dwMinorVersion;
            v.build = osvi.dwBuildNumber;
        }
        return v;
    }

    bool isWin10_1903OrGreater()
    {
        const WinVer v = queryWinVersion();
        if (v.major > 10) return true;
        if (v.major == 10 && v.build >= 18362) return true;
        return false;
    }
}
#endif

int DesktopGrabFactory::recommendedMaxFps()
{
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    /* WGC requires Windows 10 1903+. On older Win7/8/8.1/early-Win10 we fall
     * back to Qt screen-grab which is CPU-bound; cap fps to keep the box usable. */
    if (!isWin10_1903OrGreater())
    {
        return 15;
    }
#endif
    return -1;
}

std::shared_ptr<DesktopGrab> DesktopGrabFactory::createDesktopGrabber(const QString &backend, int screenIndex, QObject *parent)
{
    const QString normalized = normalizeBackend(backend);
    g_lastCreatedBackend.clear();

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    if (normalized == QStringLiteral("wgc") || normalized == QStringLiteral("auto"))
    {
        auto grabber = createPluginGrabber(QStringLiteral("wgc"), screenIndex);
        if (grabber)
        {
            g_lastCreatedBackend = QStringLiteral("wgc");
            LOG_INFO("Desktop capture backend selected: requested={}, actual=wgc", normalized);
            return grabber;
        }
    }

#endif

#ifdef AIRAN_HAS_PIPEWIRE
    /* On Linux: prefer PipeWire when Wayland is detected, since X11 grabbing
     * via Qt is faster and avoids the portal permission dialog. */
    bool tryPipeWire = (normalized == QStringLiteral("pipewire"));
    if (normalized == QStringLiteral("auto"))
    {
        const QByteArray sessionType = qgetenv("XDG_SESSION_TYPE");
        const QByteArray waylandDisplay = qgetenv("WAYLAND_DISPLAY");
        if (sessionType == QByteArrayLiteral("wayland") || !waylandDisplay.isEmpty())
            tryPipeWire = true;
    }
    if (tryPipeWire && DesktopGrabPipeWire::isAvailable())
    {
        auto pwGrabber = std::make_shared<DesktopGrabPipeWire>(parent);
        if (pwGrabber && pwGrabber->init(screenIndex))
        {
            g_lastCreatedBackend = QStringLiteral("pipewire");
            LOG_INFO("Desktop capture backend selected: requested={}, actual=pipewire", normalized);
            return pwGrabber;
        }
        LOG_WARN("PipeWire desktop capture backend failed to initialize; falling back");
    }
#endif

    if (normalized == QStringLiteral("qt") || normalized == QStringLiteral("auto"))
    {
        auto qtGrabber = std::make_shared<DesktopGrabQt>(parent);
        if (qtGrabber && qtGrabber->init(screenIndex))
        {
            g_lastCreatedBackend = QStringLiteral("qt");
            LOG_INFO("Desktop capture backend selected: requested={}, actual=qt", normalized);
            return qtGrabber;
        }
    }

    return nullptr;
}
