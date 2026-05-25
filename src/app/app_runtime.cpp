#include "app_runtime.h"

#include "app/app_style.h"
#include "common/constant.h"
#include "desktop/desktop_capture_manager.h"
#include "ui/main_window.h"
#include "util/config_util.h"
#include "util/i18n_util.h"
#include "util/json_util.h"
#include "webrtc/webrtc_cli.h"
#include "websocket/ws_cli.h"

#include <QAbstractSocket>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QHash>
#include <QHostInfo>
#include <QIcon>
#include <QMetaObject>
#include <QSharedMemory>
#include <QStringList>
#include <QThread>
#include <QTranslator>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <memory>

#ifndef AIRAN_DESK_VERSION
#define AIRAN_DESK_VERSION "0.1.0"
#endif

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace
{
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
    SERVICE_STATUS g_serviceStatus{};
    int g_serviceArgc = 0;
    char **g_serviceArgv = nullptr;
#endif

    void registerCustomTypes()
    {
        qRegisterMetaType<QAbstractSocket::SocketState>("QAbstractSocket::SocketState");
        qRegisterMetaType<rtc::PeerConnection::GatheringState>("rtc::PeerConnection::GatheringState");
        qRegisterMetaType<rtc::PeerConnection::State>("rtc::PeerConnection::State");
        qRegisterMetaType<rtc::message_variant>("rtc::message_variant");
        qRegisterMetaType<rtc::binary>("rtc::binary");
        qRegisterMetaType<std::shared_ptr<rtc::binary>>("std::shared_ptr<rtc::binary>");
        qRegisterMetaType<QJsonObject>("QJsonObject");
    }

    void initLog()
    {
        LoggerManager::instance().initialize();
        LOG_INFO("The log service was successfully initialized with spdlog.");
    }

    bool isRunning()
    {
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
        static HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\airan_mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(hMutex);
            hMutex = NULL;
            return true;
        }
        return false;
#else
        static int lockFile = -1;
        if (lockFile == -1)
        {
            QString lockPath = QDir::temp().absoluteFilePath(
                QStringLiteral("airan-%1.lock").arg(static_cast<qulonglong>(getuid())));
            lockFile = open(lockPath.toLocal8Bit().constData(), O_RDWR | O_CREAT, 0644);
        }

        return lockFile != -1 && flock(lockFile, LOCK_EX | LOCK_NB) == -1;
#endif
    }

    class HeadlessController : public QObject
    {
    public:
        explicit HeadlessController(QObject *parent = nullptr)
            : QObject(parent)
        {
            connect(&m_ws, &WsCli::onWsCliRecvBinaryMsg, this, &HeadlessController::onWsCliRecvBinaryMsg);
            connect(&m_ws, &WsCli::onWsCliRecvTextMsg, this, [this](const QString &message)
                    { onWsCliRecvBinaryMsg(message.toUtf8()); });
            m_wsThread.setObjectName(QStringLiteral("WsCliThread"));
            m_ws.moveToThread(&m_wsThread);
            m_wsThread.start();
            QMetaObject::invokeMethod(&m_ws, "init", Qt::QueuedConnection,
                                      Q_ARG(QString, buildWsUrl()),
                                      Q_ARG(quint64, 30 * 1000));
        }

        ~HeadlessController() override
        {
            cleanupWebRtcCliSessions();
            if (m_wsThread.isRunning())
                QMetaObject::invokeMethod(&m_ws, "shutdown", Qt::BlockingQueuedConnection);
            STOP_OBJ_THREAD(m_wsThread);
        }

    private:
        QString buildWsUrl() const
        {
            QUrl url(ConfigUtil->wsUrl);
            QUrlQuery query(url);
            query.removeQueryItem("sessionId");
            query.removeQueryItem("hostname");
            query.removeQueryItem("installId");
            query.addQueryItem("sessionId", ConfigUtil->local_id);
            query.addQueryItem("hostname", QHostInfo::localHostName());
            query.addQueryItem("installId", ConfigUtil->install_id);
            url.setQuery(query);
            return url.toString();
        }

        void cleanupWebRtcCliSessions()
        {
            auto sessions = m_rtcCliSessions;
            m_rtcCliSessions.clear();
            for (auto it = sessions.begin(); it != sessions.end(); ++it)
            {
                WebRtcCli *webrtcCli = it.key();
                QThread *rtcCliThread = it.value();
                if (webrtcCli && rtcCliThread && rtcCliThread->isRunning())
                {
                    QMetaObject::invokeMethod(webrtcCli, "destroy", Qt::BlockingQueuedConnection);
                    webrtcCli->disconnect();
                    QMetaObject::invokeMethod(webrtcCli, "deleteLater", Qt::BlockingQueuedConnection);
                    STOP_PTR_THREAD(rtcCliThread);
                }
                else
                {
                    DELETE_PTR_FUNC(webrtcCli);
                }
                delete rtcCliThread;
            }
        }

        void handleDeviceIdConflict(const QJsonObject &object)
        {
            const QJsonValue dataVal = object.value(Constant::KEY_DATA);
            if (!dataVal.isObject())
                return;

            const QString newSessionId = JsonUtil::getString(dataVal.toObject(), Constant::KEY_NEW_SESSION_ID);
            if (newSessionId.isEmpty() || QUuid(newSessionId).isNull())
                return;

            LOG_WARN("Device id conflict detected in headless mode, replacing local id {} -> {}",
                     ConfigUtil->local_id,
                     newSessionId);
            ConfigUtil->replaceLocalId(newSessionId);
            QMetaObject::invokeMethod(&m_ws, "resetUrlAndReconnect", Qt::QueuedConnection, Q_ARG(QString, buildWsUrl()));
        }

        void onWsCliRecvBinaryMsg(const QByteArray &message)
        {
            QJsonObject object = JsonUtil::safeParseObject(message);
            if (!JsonUtil::isValidObject(object))
                return;

            const QString sender = JsonUtil::getString(object, Constant::KEY_SENDER);
            const QString type = JsonUtil::getString(object, Constant::KEY_TYPE);
            if (sender.isEmpty() || type.isEmpty())
                return;

            if (sender == Constant::ROLE_SERVER)
            {
                if (type == Constant::TYPE_DEVICE_ID_CONFLICT)
                    handleDeviceIdConflict(object);
                else if (type == Constant::TYPE_ERROR)
                    LOG_ERROR("Server error in headless mode: {}", JsonUtil::getString(object, Constant::KEY_DATA));
                return;
            }

            if (type == Constant::TYPE_CONNECT)
            {
                const QString receiver = JsonUtil::getString(object, Constant::KEY_RECEIVER);
                if (receiver != ConfigUtil->local_id)
                    return;

                const QString receiverPwd = JsonUtil::getString(object, Constant::KEY_RECEIVER_PWD);
                if (receiverPwd.isEmpty() || receiverPwd != ConfigUtil->local_pwd_md5)
                {
                    QJsonObject errorMsg = JsonUtil::createObject()
                                               .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                                               .add(Constant::KEY_TYPE, Constant::TYPE_ERROR)
                                               .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                                               .add(Constant::KEY_RECEIVER, sender)
                                               .add(Constant::KEY_DATA, Constant::ERROR_PASSWORD_INCORRECT)
                                               .build();
                    QMetaObject::invokeMethod(&m_ws, "sendWsCliTextMsg", Qt::QueuedConnection,
                                              Q_ARG(QString, JsonUtil::toCompactString(errorMsg)));
                    return;
                }

                const int fps = JsonUtil::getInt(object, Constant::KEY_FPS, 25);
                const bool isOnlyFile = JsonUtil::getBool(object, Constant::KEY_IS_ONLY_FILE, false);
                const int requestedWidth = JsonUtil::getInt(object, Constant::KEY_WIDTH, -1);
                const int requestedHeight = JsonUtil::getInt(object, Constant::KEY_HEIGHT, -1);
                const QString qualityMode = JsonUtil::getString(object, Constant::KEY_QUALITY, QStringLiteral("quality"));
                const QString bitrateProfile = JsonUtil::getString(object, Constant::KEY_BITRATE_PROFILE, QStringLiteral("medium"));
                const QString networkPath = JsonUtil::getString(object, Constant::KEY_NETWORK_PATH, QStringLiteral("auto"));

                QThread *rtcCliThread = new QThread();
                rtcCliThread->setObjectName(QStringLiteral("WebRtcCli_%1_%2").arg(sender, isOnlyFile ? QStringLiteral("file") : QStringLiteral("desktop")));
                WebRtcCli *rtcCli = new WebRtcCli(sender, fps, isOnlyFile, requestedWidth, requestedHeight,
                                                  qualityMode, bitrateProfile, networkPath);

                connect(&m_ws, &WsCli::onWsCliRecvBinaryMsg, rtcCli, &WebRtcCli::onWsCliRecvBinaryMsg);
                connect(&m_ws, &WsCli::onWsCliRecvTextMsg, rtcCli, &WebRtcCli::onWsCliRecvTextMsg);
                connect(rtcCli, &WebRtcCli::sendWsCliBinaryMsg, &m_ws, &WsCli::sendWsCliBinaryMsg);
                connect(rtcCli, &WebRtcCli::sendWsCliTextMsg, &m_ws, &WsCli::sendWsCliTextMsg);
                connect(rtcCli, &WebRtcCli::destroyCli, this, [this, rtcCli]()
                        { destroyWebRtcCli(rtcCli); });

                rtcCli->moveToThread(rtcCliThread);
                rtcCliThread->start();
                m_rtcCliSessions.insert(rtcCli, rtcCliThread);
                QMetaObject::invokeMethod(rtcCli, "init", Qt::QueuedConnection);
            }
            else if (type == Constant::TYPE_ERROR)
            {
                LOG_ERROR("Peer error in headless mode: {}", JsonUtil::getString(object, Constant::KEY_DATA));
            }
        }

        void destroyWebRtcCli(WebRtcCli *webrtcCli)
        {
            if (!webrtcCli)
                return;

            QThread *rtcCliThread = m_rtcCliSessions.take(webrtcCli);
            if (rtcCliThread && rtcCliThread->isRunning())
            {
                QMetaObject::invokeMethod(webrtcCli, "destroy", Qt::BlockingQueuedConnection);
                webrtcCli->disconnect();
                QMetaObject::invokeMethod(webrtcCli, "deleteLater", Qt::BlockingQueuedConnection);
                STOP_PTR_THREAD(rtcCliThread);
            }
            else
            {
                DELETE_PTR_FUNC(webrtcCli);
            }
            delete rtcCliThread;
        }

        WsCli m_ws;
        QThread m_wsThread;
        QHash<WebRtcCli *, QThread *> m_rtcCliSessions;
    };

    int runApplication(int argc, char *argv[], bool forceNoUi)
    {
        registerCustomTypes();
        const bool hasDisplay = !qEnvironmentVariableIsEmpty("DISPLAY") ||
                                !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
        if (forceNoUi && !hasDisplay)
        {
            QCoreApplication::setOrganizationName("wxalh.com");
            QCoreApplication::setApplicationName("airan");
            QCoreApplication::setApplicationVersion(QStringLiteral(AIRAN_DESK_VERSION));
            QCoreApplication app(argc, argv);

            if (isRunning())
                return 0;

            ConfigUtil->showUI = false;
            ConfigUtil->language = I18nUtil::normalizeUiLanguage(ConfigUtil->language);
            const QString localeName = I18nUtil::resolveUiLanguage(ConfigUtil->language);
            QTranslator qtTranslator;
            I18nUtil::installTranslator(app, qtTranslator, QStringLiteral("qtbase_"), localeName);

            QTranslator appTranslator;
            I18nUtil::installTranslator(app, appTranslator, QStringLiteral("airan-desk_"), localeName);

            initLog();
            std::unique_ptr<HeadlessController> headlessController = std::make_unique<HeadlessController>();

            const int result = app.exec();
            LOG_DEBUG("Application is exiting...");
            rtc::Cleanup();
            return result;
        }

#if (QT_VERSION >= QT_VERSION_CHECK(5, 1, 2))
        QApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
        QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
#endif

#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
        QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::Round);
#endif

        QApplication::setOrganizationName("wxalh.com");
        QApplication::setApplicationName("airan");
        QApplication::setApplicationVersion(QStringLiteral(AIRAN_DESK_VERSION));
        QGuiApplication::setDesktopFileName(QStringLiteral("airan-desk"));
        QApplication app(argc, argv);
        app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));
        applyGlobalStyle(app);

        if (isRunning())
            return 0;

        if (forceNoUi)
            ConfigUtil->showUI = false;

        ConfigUtil->language = I18nUtil::normalizeUiLanguage(ConfigUtil->language);
        const QString localeName = I18nUtil::resolveUiLanguage(ConfigUtil->language);
        QTranslator qtTranslator;
        I18nUtil::installTranslator(app, qtTranslator, QStringLiteral("qtbase_"), localeName);

        QTranslator appTranslator;
        I18nUtil::installTranslator(app, appTranslator, QStringLiteral("airan-desk_"), localeName);

        initLog();
        DesktopCaptureManager::instance();

        std::unique_ptr<MainWindow> mainWindow;
        std::unique_ptr<HeadlessController> headlessController;
        if (ConfigUtil->showUI)
        {
            mainWindow = std::make_unique<MainWindow>();
            mainWindow->show();
        }
        else
        {
            headlessController = std::make_unique<HeadlessController>();
        }

        const int result = app.exec();
        LOG_DEBUG("Application is exiting...");
        rtc::Cleanup();
        return result;
    }

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    void reportServiceStatus(DWORD currentState, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0)
    {
        if (!g_serviceStatusHandle)
            return;

        g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        g_serviceStatus.dwCurrentState = currentState;
        g_serviceStatus.dwWin32ExitCode = win32ExitCode;
        g_serviceStatus.dwWaitHint = waitHint;
        g_serviceStatus.dwControlsAccepted =
            currentState == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    }

    void WINAPI serviceCtrlHandler(DWORD control)
    {
        if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
        {
            reportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
            if (qApp)
                QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
        }
    }

    void WINAPI serviceMain(DWORD, LPWSTR *)
    {
        g_serviceStatusHandle = RegisterServiceCtrlHandlerW(L"airan-desk", serviceCtrlHandler);
        if (!g_serviceStatusHandle)
            return;

        reportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
        reportServiceStatus(SERVICE_RUNNING);
        const int result = runApplication(g_serviceArgc, g_serviceArgv, true);
        reportServiceStatus(SERVICE_STOPPED, static_cast<DWORD>(result));
    }
#endif
}

int runAiranDesk(int argc, char *argv[])
{
    QStringList args;
    for (int i = 0; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    const bool serviceMode = args.contains(QStringLiteral("--service"));
    const bool forceNoUi = serviceMode || args.contains(QStringLiteral("--no-ui"));

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    if (serviceMode)
    {
        g_serviceArgc = argc;
        g_serviceArgv = argv;
        SERVICE_TABLE_ENTRYW dispatchTable[] = {
            {const_cast<LPWSTR>(L"airan-desk"), serviceMain},
            {nullptr, nullptr}};
        if (StartServiceCtrlDispatcherW(dispatchTable))
            return 0;
        if (GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            return static_cast<int>(GetLastError());
    }
#endif

    return runApplication(argc, argv, forceNoUi);
}
