/* Split from webrtc_cli.cpp by client-side responsibility. */

#include "webrtc_cli.h"
#include "../util/file_packet_util.h"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QUuid>

WebRtcCli::WebRtcCli(const QString &remoteId, int fps, bool isOnlyFile, QObject *parent)
    : WebRtcCli(remoteId, fps, isOnlyFile, -1, -1, QStringLiteral("quality"), QStringLiteral("medium"), QStringLiteral("auto"), parent)
{
}

WebRtcCli::WebRtcCli(const QString &remoteId, int fps, bool isOnlyFile, int requestedWidth, int requestedHeight,
                     const QString &qualityMode, const QString &bitrateProfile, const QString &networkPath, QObject *parent)
    : QObject(parent),
      m_remoteId(remoteId),
      m_subscriberId(QUuid::createUuid().toString().remove("{").remove("}")),
      m_isOnlyFile(isOnlyFile), /* 默认不是仅文件传输 */
      m_currentDir(QDir::home()),
      m_connected(false),
      m_channelsReady(false),
      m_destroying(false),
      m_fps(fps),
      m_subscribed(false)
{
#if QT_POINTER_SIZE == 4
    if (!m_isOnlyFile && m_fps > 15)
    {
        m_fps = 15;
        LOG_WARN("32-bit controlled process / CPU encode path detected, cap capture fps to {}", m_fps);
    }
#endif

    QGuiApplication *guiApp = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    QScreen *screen = guiApp ? guiApp->primaryScreen() : nullptr;
    QRect screenGeometry = screen ? screen->geometry() : QRect(0, 0, 1920, 1080);
    m_screen_width = screenGeometry.width();
    m_screen_height = screenGeometry.height();

    m_requestedEncodeWidth = requestedWidth;
    m_requestedEncodeHeight = requestedHeight;
    m_baseRequestedEncodeWidth = requestedWidth;
    m_baseRequestedEncodeHeight = requestedHeight;
    calculateEncodeResolution(m_requestedEncodeWidth, m_requestedEncodeHeight);
    m_bitrateProfile = bitrateProfile.isEmpty() ? QStringLiteral("medium") : bitrateProfile;
    m_baseBitrateProfile = m_bitrateProfile;
    m_baseFps = m_fps;
    m_networkPath = networkPath.isEmpty() ? QStringLiteral("auto") : networkPath;
    setStreamMode(qualityMode);

    /* 初始化ICE服务器配置 */
    m_host = ConfigUtil->ice_host.toStdString();
    m_port = (uint16_t)ConfigUtil->ice_port;
    m_username = ConfigUtil->ice_username.toStdString();
    m_password = ConfigUtil->ice_password.toStdString();

    /* 初始化文件分包工具类（由 QObject 父对象管理生命周期） */
    m_filePacketUtil = new FilePacketUtil(this);

    /* 连接文件接收信号 */
    connect(m_filePacketUtil, &FilePacketUtil::fileDownloadCompleted, this, &WebRtcCli::handleFileReceived);
    connect(m_filePacketUtil, &FilePacketUtil::fileReceived, this, &WebRtcCli::handleFileReceived);

    /* 输入通道恢复定时器（防抖，避免弱网抖动时重协商风暴） */
    m_inputChannelRecoverTimer = new QTimer(this);
    m_inputChannelRecoverTimer->setSingleShot(true);
    connect(m_inputChannelRecoverTimer, &QTimer::timeout, this, &WebRtcCli::recoverInputChannel);

    m_videoDataChannelRecoverTimer = new QTimer(this);
    m_videoDataChannelRecoverTimer->setSingleShot(true);
    connect(m_videoDataChannelRecoverTimer, &QTimer::timeout, this, &WebRtcCli::recoverVideoDataChannel);

    m_controlWatchdogTimer = new QTimer(this);
    connect(m_controlWatchdogTimer, &QTimer::timeout, this, &WebRtcCli::checkControlAlive);
    m_disconnectGraceTimer = new QTimer(this);
    m_disconnectGraceTimer->setSingleShot(true);
    connect(m_disconnectGraceTimer, &QTimer::timeout, this, &WebRtcCli::stopMediaCapture);

    LOG_INFO("created for remote: {}, subscriber: {}", m_remoteId, m_subscriberId);
}

WebRtcCli::~WebRtcCli()
{
    LOG_DEBUG("WebRtcCli destructor");

    /* 先调用destroy停止所有活动 */
    destroy();
}

void WebRtcCli::init()
{
    LOG_INFO("Creating PeerConnection and tracks for client side");
    /* 初始化WebRTC */
    initPeerConnection();
    if (!m_peerConnection)
    {
        LOG_ERROR("Client init aborted: PeerConnection is not available");
        emit destroyCli();
        return;
    }

    setupCallbacks();
    /* 创建轨道和数据通道 */
    createTracksAndChannels();
    if (!m_isOnlyFile && (!m_videoTrack || !m_audioTrack || !m_inputChannel))
    {
        LOG_ERROR("Client init aborted: media tracks or input channel were not created");
        emit destroyCli();
        return;
    }
    if (!m_fileChannel || !m_fileTextChannel)
    {
        LOG_ERROR("Client init aborted: file data channels were not created");
        emit destroyCli();
        return;
    }
    m_peerConnection->createOffer();
}
