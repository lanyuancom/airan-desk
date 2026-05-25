/* Wayland / xdg-desktop-portal + PipeWire screen-capture implementation.
 *
 * Flow (xdg-desktop-portal ScreenCast):
 *   1. CreateSession  -> returns session handle
 *   2. SelectSources  -> chooses monitor type
 *   3. Start          -> emits Response with streams[node_id]
 *   4. OpenPipeWireRemote -> returns FD for pw_core_connect_fd
 *   5. pw_stream_connect on the node id, consume RGBx frames into QImage
 *
 * Build is gated by AIRAN_HAS_PIPEWIRE (set in CMake when libpipewire-0.3 is found).
 */

#include "desktop_grab_pipewire.h"

#ifdef AIRAN_HAS_PIPEWIRE

#include "../common/constant.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QEventLoop>
#include <QRandomGenerator>
#include <QTimer>
#include <QVariantMap>

#include <pipewire/pipewire.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>

namespace
{
constexpr const char *kPortalService     = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath        = "/org/freedesktop/portal/desktop";
constexpr const char *kPortalScreenCast  = "org.freedesktop.portal.ScreenCast";
constexpr const char *kPortalRequest     = "org.freedesktop.portal.Request";

QString genToken(const char *prefix)
{
    return QStringLiteral("%1_%2").arg(prefix).arg(QRandomGenerator::global()->generate());
}

QString requestPathFor(const QString &handleToken, const QString &uniqueName)
{
    QString sender = uniqueName;
    sender.remove(0, 1).replace('.', '_');
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, handleToken);
}
} /* namespace */

/* Helper QObject so we can use QDBusConnection::connect() with a real slot
 * (Qt5's lambda overload requires Q_OBJECT-marked types). Lives at file scope
 * because Q_OBJECT does not work inside anonymous namespaces. */
class PortalResponseWaiter : public QObject
{
    Q_OBJECT
public:
    explicit PortalResponseWaiter(QObject *parent = nullptr) : QObject(parent) {}
    quint32 response{1};
    QVariantMap results;
    bool fired{false};
public slots:
    void onResponse(quint32 resp, const QVariantMap &res)
    {
        response = resp;
        results = res;
        fired = true;
        emit done();
    }
signals:
    void done();
};

namespace
{
/* Block on the portal Response signal for a single Request handle. */
bool waitForPortalResponse(QDBusConnection &bus, const QString &requestPath, QVariantMap &outResults, int timeoutMs = 60000)
{
    QEventLoop loop;
    PortalResponseWaiter waiter;
    QObject::connect(&waiter, &PortalResponseWaiter::done, &loop, &QEventLoop::quit);

    bool connected = bus.connect(kPortalService, requestPath, kPortalRequest, QStringLiteral("Response"),
                                 &waiter, SLOT(onResponse(quint32, QVariantMap)));
    if (!connected)
    {
        LOG_ERROR("Failed to subscribe to portal Response on {}", requestPath.toStdString());
        return false;
    }

    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();

    bus.disconnect(kPortalService, requestPath, kPortalRequest, QStringLiteral("Response"),
                   &waiter, SLOT(onResponse(quint32, QVariantMap)));

    if (!waiter.fired)
    {
        LOG_ERROR("xdg-desktop-portal Response timeout for {}", requestPath.toStdString());
        return false;
    }
    if (waiter.response != 0)
    {
        LOG_ERROR("xdg-desktop-portal call rejected, response={}", waiter.response);
        return false;
    }
    outResults = waiter.results;
    return true;
}

QImage::Format pwFormatToQImage(uint32_t pwFmt)
{
    switch (pwFmt)
    {
    case SPA_VIDEO_FORMAT_RGBA: return QImage::Format_RGBA8888;
    case SPA_VIDEO_FORMAT_BGRA: return QImage::Format_ARGB32;       /* little-endian B,G,R,A == ARGB32 in memory */
    case SPA_VIDEO_FORMAT_RGBx: return QImage::Format_RGBX8888;
    case SPA_VIDEO_FORMAT_BGRx: return QImage::Format_RGB32;
    default: return QImage::Format_Invalid;
    }
}
} /* namespace */

bool DesktopGrabPipeWire::isAvailable()
{
    /* Minimal probe: PipeWire and the portal service must be accessible. */
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;
    QDBusInterface iface(kPortalService, kPortalPath, kPortalScreenCast, bus);
    if (!iface.isValid())
        return false;
    return true;
}

DesktopGrabPipeWire::DesktopGrabPipeWire(QObject *parent)
    : DesktopGrab(parent)
{
    pw_init(nullptr, nullptr);
}

DesktopGrabPipeWire::~DesktopGrabPipeWire()
{
    teardown();
}

bool DesktopGrabPipeWire::init(int screenIndex)
{
    Q_UNUSED(screenIndex); /* portal lets the user choose; we honor that selection. */
    if (!startPortalSession())
    {
        LOG_ERROR("Failed to negotiate xdg-desktop-portal ScreenCast session");
        return false;
    }
    if (!connectPipeWire())
    {
        LOG_ERROR("Failed to connect to PipeWire stream node {}", m_nodeId);
        teardown();
        return false;
    }
    LOG_INFO("PipeWire ScreenCast initialized: node={}", m_nodeId);
    return true;
}

bool DesktopGrabPipeWire::startPortalSession()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
    {
        LOG_ERROR("D-Bus session bus not available; cannot use xdg-desktop-portal");
        return false;
    }

    const QString uniqueName = bus.baseService();

    /* 1. CreateSession */
    {
        const QString reqToken     = genToken("airan_req");
        const QString sessionToken = genToken("airan_sess");
        const QString reqPath      = requestPathFor(reqToken, uniqueName);

        QVariantMap opts;
        opts.insert(QStringLiteral("handle_token"), reqToken);
        opts.insert(QStringLiteral("session_handle_token"), sessionToken);

        QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kPortalScreenCast,
                                                            QStringLiteral("CreateSession"));
        call << opts;
        QDBusMessage reply = bus.call(call);
        if (reply.type() == QDBusMessage::ErrorMessage)
        {
            LOG_ERROR("CreateSession failed: {}", reply.errorMessage().toStdString());
            return false;
        }

        QVariantMap results;
        if (!waitForPortalResponse(bus, reqPath, results))
            return false;
        m_portalSessionPath = qdbus_cast<QString>(results.value(QStringLiteral("session_handle")));
        if (m_portalSessionPath.isEmpty())
        {
            LOG_ERROR("Portal returned empty session_handle");
            return false;
        }
    }

    /* 2. SelectSources (monitors only) */
    {
        const QString reqToken = genToken("airan_req");
        const QString reqPath  = requestPathFor(reqToken, uniqueName);

        QVariantMap opts;
        opts.insert(QStringLiteral("handle_token"), reqToken);
        opts.insert(QStringLiteral("types"), quint32(1)); /* MONITOR */
        opts.insert(QStringLiteral("multiple"), false);
        opts.insert(QStringLiteral("cursor_mode"), quint32(2)); /* embedded */

        QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kPortalScreenCast,
                                                            QStringLiteral("SelectSources"));
        call << QVariant::fromValue(QDBusObjectPath(m_portalSessionPath)) << opts;
        QDBusMessage reply = bus.call(call);
        if (reply.type() == QDBusMessage::ErrorMessage)
        {
            LOG_ERROR("SelectSources failed: {}", reply.errorMessage().toStdString());
            return false;
        }
        QVariantMap results;
        if (!waitForPortalResponse(bus, reqPath, results))
            return false;
    }

    /* 3. Start */
    {
        const QString reqToken = genToken("airan_req");
        const QString reqPath  = requestPathFor(reqToken, uniqueName);

        QVariantMap opts;
        opts.insert(QStringLiteral("handle_token"), reqToken);

        QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kPortalScreenCast,
                                                            QStringLiteral("Start"));
        call << QVariant::fromValue(QDBusObjectPath(m_portalSessionPath))
             << QString() /* parent_window */
             << opts;
        QDBusMessage reply = bus.call(call);
        if (reply.type() == QDBusMessage::ErrorMessage)
        {
            LOG_ERROR("Start failed: {}", reply.errorMessage().toStdString());
            return false;
        }
        QVariantMap results;
        if (!waitForPortalResponse(bus, reqPath, results))
            return false;

        /* streams: a(ua{sv}) — array of (node_id, properties) */
        QDBusArgument arg = results.value(QStringLiteral("streams")).value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd())
        {
            arg.beginStructure();
            quint32 nodeId = 0;
            QVariantMap props;
            arg >> nodeId >> props;
            arg.endStructure();
            if (m_nodeId == 0)
                m_nodeId = nodeId;
        }
        arg.endArray();
        if (m_nodeId == 0)
        {
            LOG_ERROR("Portal Start returned no streams");
            return false;
        }
    }

    /* 4. OpenPipeWireRemote -> fd */
    {
        QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, kPortalPath, kPortalScreenCast,
                                                            QStringLiteral("OpenPipeWireRemote"));
        call << QVariant::fromValue(QDBusObjectPath(m_portalSessionPath)) << QVariantMap();
        QDBusMessage reply = bus.call(call);
        if (reply.type() == QDBusMessage::ErrorMessage)
        {
            LOG_ERROR("OpenPipeWireRemote failed: {}", reply.errorMessage().toStdString());
            return false;
        }
        if (reply.arguments().isEmpty())
        {
            LOG_ERROR("OpenPipeWireRemote returned no fd");
            return false;
        }
        QDBusUnixFileDescriptor ufd = qvariant_cast<QDBusUnixFileDescriptor>(reply.arguments().first());
        if (!ufd.isValid())
        {
            LOG_ERROR("OpenPipeWireRemote returned invalid fd");
            return false;
        }
        m_pwFd = ::dup(ufd.fileDescriptor());
    }

    return true;
}

void DesktopGrabPipeWire::onStreamProcessStatic(void *userdata)
{
    static_cast<DesktopGrabPipeWire *>(userdata)->onStreamProcess();
}

void DesktopGrabPipeWire::onStreamParamChangedStatic(void *userdata, uint32_t id, const struct spa_pod *param)
{
    static_cast<DesktopGrabPipeWire *>(userdata)->onStreamParamChanged(id, param);
}

void DesktopGrabPipeWire::onStreamStateChangedStatic(void *userdata, int oldState, int newState, const char *error)
{
    Q_UNUSED(userdata);
    LOG_INFO("PipeWire stream state: {} -> {}{}", oldState, newState, error ? error : "");
}

void DesktopGrabPipeWire::onStreamParamChanged(uint32_t id, const struct spa_pod *param)
{
    if (!param || id != SPA_PARAM_Format)
        return;
    struct spa_video_info info{};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;
    if (info.media_type != SPA_MEDIA_TYPE_video || info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0)
        return;

    QMutexLocker lock(&m_frameMutex);
    m_frameWidth  = info.info.raw.size.width;
    m_frameHeight = info.info.raw.size.height;
    m_frameFormat = info.info.raw.format;
    m_screen_width  = m_frameWidth;
    m_screen_height = m_frameHeight;
    LOG_INFO("PipeWire stream format negotiated: {}x{} fmt={}", m_frameWidth, m_frameHeight, m_frameFormat);
}

void DesktopGrabPipeWire::onStreamProcess()
{
    if (!m_stream || m_stopping.load())
        return;
    pw_buffer *b = pw_stream_dequeue_buffer(m_stream);
    if (!b)
        return;
    spa_buffer *buf = b->buffer;
    if (buf && buf->datas[0].data)
    {
        const uint32_t stride = buf->datas[0].chunk->stride;
        const uint8_t *src = static_cast<const uint8_t *>(buf->datas[0].data);

        QMutexLocker lock(&m_frameMutex);
        QImage::Format qfmt = pwFormatToQImage(m_frameFormat);
        if (qfmt != QImage::Format_Invalid && m_frameWidth > 0 && m_frameHeight > 0)
        {
            QImage img(m_frameWidth, m_frameHeight, qfmt);
            const int dstStride = img.bytesPerLine();
            for (int y = 0; y < m_frameHeight; ++y)
            {
                memcpy(img.scanLine(y), src + y * stride, std::min<int>(stride, dstStride));
            }
            m_latest = std::move(img);
            m_frameStride = dstStride;
            m_haveFrame.store(true);
        }
    }
    pw_stream_queue_buffer(m_stream, b);
}

bool DesktopGrabPipeWire::connectPipeWire()
{
    m_loop = pw_thread_loop_new("airan-pipewire", nullptr);
    if (!m_loop)
        return false;
    pw_thread_loop_lock(m_loop);

    m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
    if (!m_context)
    {
        pw_thread_loop_unlock(m_loop);
        return false;
    }
    m_core = pw_context_connect_fd(m_context, m_pwFd, nullptr, 0);
    m_pwFd = -1; /* core owns it */
    if (!m_core)
    {
        pw_thread_loop_unlock(m_loop);
        return false;
    }

    static const pw_stream_events streamEvents = []{
        pw_stream_events ev{};
        ev.version = PW_VERSION_STREAM_EVENTS;
        ev.process = &DesktopGrabPipeWire::onStreamProcessStatic;
        ev.param_changed = &DesktopGrabPipeWire::onStreamParamChangedStatic;
        ev.state_changed = &DesktopGrabPipeWire::onStreamStateChangedStatic;
        return ev;
    }();

    m_stream = pw_stream_new(m_core, "airan-screencast",
                             pw_properties_new(PW_KEY_MEDIA_TYPE,     "Video",
                                               PW_KEY_MEDIA_CATEGORY, "Capture",
                                               PW_KEY_MEDIA_ROLE,     "Screen",
                                               nullptr));
    if (!m_stream)
    {
        pw_thread_loop_unlock(m_loop);
        return false;
    }
    if (!m_streamListener)
        m_streamListener = new spa_hook{};
    pw_stream_add_listener(m_stream, m_streamListener, &streamEvents, this);

    /* Format params: BGRx/BGRA/RGBx/RGBA, range of resolutions. */
    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod *params[1];
    spa_rectangle minRes = SPA_RECTANGLE(64,  64);
    spa_rectangle defRes = SPA_RECTANGLE(1920, 1080);
    spa_rectangle maxRes = SPA_RECTANGLE(7680, 4320);
    spa_fraction minFps  = SPA_FRACTION(1, 1);
    spa_fraction defFps  = SPA_FRACTION(30, 1);
    spa_fraction maxFps  = SPA_FRACTION(120, 1);

    params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
        &b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
                                                        SPA_VIDEO_FORMAT_BGRx,
                                                        SPA_VIDEO_FORMAT_RGBx,
                                                        SPA_VIDEO_FORMAT_BGRA,
                                                        SPA_VIDEO_FORMAT_RGBA,
                                                        SPA_VIDEO_FORMAT_BGRx),
        SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(&defRes, &minRes, &maxRes),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defFps,  &minFps,  &maxFps)));

    int ret = pw_stream_connect(m_stream, PW_DIRECTION_INPUT, m_nodeId,
                                static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                              PW_STREAM_FLAG_MAP_BUFFERS),
                                params, 1);
    if (ret < 0)
    {
        LOG_ERROR("pw_stream_connect failed: {}", spa_strerror(ret));
        pw_thread_loop_unlock(m_loop);
        return false;
    }
    pw_thread_loop_unlock(m_loop);
    pw_thread_loop_start(m_loop);
    return true;
}

bool DesktopGrabPipeWire::grabFrameCPU(QImage &outImage)
{
    if (m_stopping.load() || !m_haveFrame.load())
        return false;
    QMutexLocker lock(&m_frameMutex);
    if (m_latest.isNull())
        return false;
    outImage = m_latest.copy();
    return true;
}

void DesktopGrabPipeWire::stopCapture()
{
    teardown();
}

void DesktopGrabPipeWire::teardown()
{
    m_stopping.store(true);
    if (m_loop)
    {
        pw_thread_loop_stop(m_loop);
        if (m_stream)
        {
            pw_stream_destroy(m_stream);
            m_stream = nullptr;
        }
        if (m_streamListener)
        {
            delete m_streamListener;
            m_streamListener = nullptr;
        }
        if (m_core)
        {
            pw_core_disconnect(m_core);
            m_core = nullptr;
        }
        if (m_context)
        {
            pw_context_destroy(m_context);
            m_context = nullptr;
        }
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
    }
    if (m_pwFd >= 0)
    {
        ::close(m_pwFd);
        m_pwFd = -1;
    }
    if (!m_portalSessionPath.isEmpty())
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (bus.isConnected())
        {
            QDBusMessage call = QDBusMessage::createMethodCall(kPortalService, m_portalSessionPath,
                                                                QStringLiteral("org.freedesktop.portal.Session"),
                                                                QStringLiteral("Close"));
            bus.call(call);
        }
        m_portalSessionPath.clear();
    }
    m_nodeId = 0;
    m_haveFrame.store(false);
}

#endif /* AIRAN_HAS_PIPEWIRE */

#ifdef AIRAN_HAS_PIPEWIRE
#include "desktop_grab_pipewire.moc"
#endif
