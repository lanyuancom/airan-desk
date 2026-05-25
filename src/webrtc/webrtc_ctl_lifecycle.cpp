/* Split from webrtc_ctl.cpp by WebRTC control-side responsibility. */

#include "webrtc_ctl.h"
#include "../codec/h264_decoder.h"
#include "../util/file_packet_util.h"
#include "../util/json_util.h"

#include <QTimer>
#include <QThread>

WebRtcCtl::WebRtcCtl(const QString &remoteId, const QString &remotePwdMd5,
                     bool isOnlyFile, QObject *parent)
    : QObject(parent),
      m_remoteId(remoteId),
      m_remotePwdMd5(remotePwdMd5),
      m_connected(false),
      m_isOnlyFile(isOnlyFile)
{
    /* 初始化ICE服务器配置 */
    m_host = ConfigUtil->ice_host.toStdString();
    m_port = (uint16_t)ConfigUtil->ice_port;
    m_username = ConfigUtil->ice_username.toStdString();
    m_password = ConfigUtil->ice_password.toStdString();
    m_streamMode = ConfigUtil->remote_quality;
    m_acceptReliableVideo.store(m_streamMode == QStringLiteral("quality") || m_streamMode == QStringLiteral("compat"));
    m_bitrateProfile = ConfigUtil->remote_bitrate_profile;
    m_networkPath = ConfigUtil->remote_network_path;
    m_captureBackend = ConfigUtil->remote_capture_backend;
    m_requestedWidth = ConfigUtil->remote_width;
    m_requestedHeight = ConfigUtil->remote_height;

    /* 初始化文件分包工具 */
    m_filePacketUtil = std::make_unique<FilePacketUtil>(this);

    /* 连接文件分包工具的信号 */
    connect(m_filePacketUtil.get(), &FilePacketUtil::fileDownloadCompleted,
            this, &WebRtcCtl::recvDownloadFile);
    connect(m_filePacketUtil.get(), &FilePacketUtil::fileReceived,
            this, &WebRtcCtl::recvDownloadFile);

    /* 重连定时器（单次触发） */
    m_reconnectTimer = new QTimer(this);
    m_controlHeartbeatTimer = new QTimer(this);
    connect(m_reconnectTimer, &QTimer::timeout, this, &WebRtcCtl::doReconnect);
    connect(m_controlHeartbeatTimer, &QTimer::timeout, this, &WebRtcCtl::sendControlHeartbeat);

    LOG_INFO("created for remote: {}", m_remoteId);
}

WebRtcCtl::~WebRtcCtl()
{
    LOG_DEBUG("destructor");
    if (!m_shutdownDone && QThread::currentThread() == thread())
        shutdown();
}

void WebRtcCtl::shutdown()
{
    if (m_shutdownDone)
        return;

    LOG_DEBUG("WebRtcCtl shutdown");
    m_shutdownDone = true;
    m_allowReconnect = false;
    m_reconnectPending.store(false);
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    if (m_controlHeartbeatTimer)
        m_controlHeartbeatTimer->stop();
    destroy();
}

void WebRtcCtl::init()
{
    LOG_INFO("Creating PeerConnection for control side");
    m_availableNetworkPaths = QStringList() << QStringLiteral("auto");
    m_selectedNetworkPath.clear();
    publishNetworkPathState();

    if (!m_isOnlyFile)
    {
        /* 初始化H264解码器（启用硬件加速） */
        m_h264Decoder = std::make_unique<H264Decoder>(this);
        if (!m_h264Decoder->initialize())
        {
            LOG_ERROR("Failed to initialize H264 decoder");
            m_h264Decoder = nullptr; /* 确保解码器不可用 */
            return;
        }
    }
    /* 初始化WebRTC */
    initPeerConnection();
    if (!m_peerConnection)
    {
        LOG_ERROR("Control init aborted: PeerConnection is not available");
        scheduleReconnect();
        return;
    }
    if (!m_isOnlyFile)
    {
        /* 创建接收轨道 */
        createTracks();
        if (!m_videoTrack)
        {
            LOG_ERROR("Control init aborted: video track was not created");
            scheduleReconnect();
            return;
        }
    }

    setupCallbacks();

    /* 发送CONNECT消息给被控端 */
    int requestedFps = ConfigUtil->fps;
#if QT_POINTER_SIZE == 4
    if (!m_isOnlyFile && requestedFps > 10)
    {
        requestedFps = 10;
        LOG_WARN("32-bit control process detected, cap remote desktop fps to {} to reduce CPU decode/render pressure", requestedFps);
    }
#endif

    auto connectBuilder = JsonUtil::createObject()
                              .add(Constant::KEY_ROLE, Constant::ROLE_CTL)
                              .add(Constant::KEY_TYPE, Constant::TYPE_CONNECT)
                              .add(Constant::KEY_RECEIVER, m_remoteId)
                              .add(Constant::KEY_RECEIVER_PWD, m_remotePwdMd5)
                              .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                              .add(Constant::KEY_IS_ONLY_FILE, m_isOnlyFile)
                              .add(Constant::KEY_WIDTH, m_requestedWidth)
                              .add(Constant::KEY_HEIGHT, m_requestedHeight)
                              .add(Constant::KEY_QUALITY, m_streamMode)
                              .add(Constant::KEY_BITRATE_PROFILE, m_bitrateProfile)
                              .add(Constant::KEY_NETWORK_PATH, m_networkPath)
                              .add(Constant::KEY_CAPTURE_BACKEND, m_captureBackend)
                              .add(Constant::KEY_FPS, requestedFps);

    QJsonObject connectMsg = connectBuilder.build();
    LOG_INFO("Sending CONNECT message with initial stream constraints: quality={}, bitrateProfile={}, networkPath={}, fps={}",
             m_streamMode, m_bitrateProfile, m_networkPath, requestedFps);
    QString message = JsonUtil::toCompactString(connectMsg);
    emit sendWsCliTextMsg(message);
}
