/* Split from webrtc_ctl.cpp by WebRTC control-side responsibility. */

#include "webrtc_ctl.h"
#include "opus_duplex_media_handler.h"
#include "sdp_quality_patcher.h"
#include "../common/constant.h"
#include "../util/json_util.h"
#include "../util/qt_callback_util.h"

#include <utility>

#include "webrtc_ctl.h"
#include "../common/constant.h"
#include "../codec/h264_decoder.h"
#include "../util/json_util.h"
#include "../util/file_packet_util.h"
#include "../util/qt_callback_util.h"
#include <QTimer>
#include <QThread>
#include <QDataStream>
#include <QUuid>
#include <QDateTime>
#include <iostream>
#include <QPointer>
#include <algorithm>
#include <utility>

namespace
{
QString networkPathFromCandidate(const QString &candidate)
{
    const QString normalized = candidate.simplified().toLower();
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QStringList parts = normalized.split(' ', Qt::SkipEmptyParts);
#else
    const QStringList parts = normalized.split(' ', QString::SkipEmptyParts);
#endif
    const QString protocol = parts.size() > 2 ? parts.at(2) : QString();

    if (normalized.contains(QStringLiteral(" typ relay")))
        return protocol == QStringLiteral("tcp") ? QStringLiteral("turn_tcp") : QStringLiteral("turn_udp");

    if (normalized.contains(QStringLiteral(" typ host")) ||
        normalized.contains(QStringLiteral(" typ srflx")) ||
        normalized.contains(QStringLiteral(" typ prflx")))
        return QStringLiteral("direct");

    return QString();
}

QString selectedNetworkPathFromPair(const QString &localCandidate, const QString &remoteCandidate)
{
    const QString localPath = networkPathFromCandidate(localCandidate);
    const QString remotePath = networkPathFromCandidate(remoteCandidate);

    if (localPath == QStringLiteral("turn_tcp") || remotePath == QStringLiteral("turn_tcp"))
        return QStringLiteral("turn_tcp");
    if (localPath == QStringLiteral("turn_udp") || remotePath == QStringLiteral("turn_udp"))
        return QStringLiteral("turn_udp");
    if (localPath == QStringLiteral("direct") || remotePath == QStringLiteral("direct"))
        return QStringLiteral("direct");
    return QString();
}
} /* namespace */

/**
 * WebRtcCtl类实现
 * 该类负责处理WebRTC客户端的所有功能，包括连接、媒体处理、数据
 * init pc -> setup tracks and datachannels -> on recv remote sdp ->send remote sdp -> send local sdp
 * -> on remote ice candidates -> send local ice candidates
 */
void WebRtcCtl::initPeerConnection()
{
    try
    {
        rtc::Configuration config = buildRtcConfiguration();

        /* 创建PeerConnection */
        m_peerConnection = std::make_shared<rtc::PeerConnection>(config);
        LOG_INFO("PeerConnection created successfully, networkPath={}", m_networkPath);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to initialize PeerConnection: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Failed to initialize PeerConnection: unknown error");
    }
}

rtc::Configuration WebRtcCtl::buildRtcConfiguration() const
{
    rtc::Configuration config;
    const QString path = m_networkPath.toLower();
    const bool hasTurnCreds = !m_username.empty() && !m_password.empty();
    const bool forceRelay = (path == QStringLiteral("turn_udp") || path == QStringLiteral("turn_tcp"));

    if (forceRelay && !hasTurnCreds)
    {
        LOG_WARN("TURN relay path requested but TURN credentials are empty; falling back to direct/STUN candidates");
        config.iceServers.push_back(rtc::IceServer(m_host, m_port));
        return config;
    }

    if (path == QStringLiteral("direct") || path == QStringLiteral("auto"))
        config.iceServers.push_back(rtc::IceServer(m_host, m_port));

    if ((path == QStringLiteral("turn_udp") || path == QStringLiteral("auto")) && hasTurnCreds)
        config.iceServers.push_back(rtc::IceServer(m_host, m_port, m_username, m_password, rtc::IceServer::RelayType::TurnUdp));

    if ((path == QStringLiteral("turn_tcp") || path == QStringLiteral("auto")) && hasTurnCreds)
    {
        config.iceServers.push_back(rtc::IceServer(m_host, m_port, m_username, m_password, rtc::IceServer::RelayType::TurnTcp));
        config.enableIceTcp = true;
    }

    if (forceRelay)
        config.iceTransportPolicy = rtc::TransportPolicy::Relay;

    return config;
}

void WebRtcCtl::createTracks()
{
    if (!m_peerConnection)
    {
        LOG_ERROR("PeerConnection not available for creating tracks");
        return;
    }

    try
    {
        /* 创建视频接收轨道 - 设置RTP解包器 */
        LOG_INFO("Creating video receive track");
        std::string video_name = Constant::TYPE_VIDEO.toStdString();
        rtc::Description::Video videoDesc(video_name); /* 使用固定流名称匹配发送端 */
        videoDesc.addH264Codec(96);                    /* 确保payload type匹配 */
        uint32_t videoSSRC = 1;
        std::string msid = Constant::TYPE_VIDEO_MSID.toStdString();
        videoDesc.addSSRC(videoSSRC, video_name, msid, video_name);
        videoDesc.setDirection(rtc::Description::Direction::RecvOnly);
        m_videoTrack = m_peerConnection->addTrack(videoDesc);

        /* 为视频轨道设置H264 RTP解包器 - 这是必需的！ */
        auto h264Depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
        m_videoTrack->setMediaHandler(h264Depacketizer);
        /* 必须链上 RTCP 接收会话：它会统计 RTP 丢包并发送 RR/REMB/PLI。 */
        /* 发送端已经挂了 RtcpNackResponder，但接收端没有 RtcpReceivingSession 时， */
        /* 丢包后不会主动反馈/请求关键帧，H264 的 P 帧错误会持续传播，表现为花屏。 */
        m_videoTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
        /* requestBitrate 需要 Track 进入 open 状态后才可靠，部分平台/版本在建轨阶段调用会抛 */
        /* “Track is not open”，导致后续音频轨道创建被中断；这里交由 RTCP REMB/PLI 在连接后协商。 */

        if (m_audioMode != QStringLiteral("off"))
        {
            /* 默认关闭音频时不参与首轮 SDP，避免旧版 Android 在 m=audio 上 RTCP mux 协商失败。 */
            ensureAudioTrack();
        }

        LOG_INFO("Control side tracks created successfully");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to create tracks: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Failed to create tracks: unknown error");
    }
}

void WebRtcCtl::setupCallbacks()
{
    if (!m_peerConnection)
        return;

    m_peerConnection->onStateChange(makeWeakCallback(this, &WebRtcCtl::onPeerConnectionStateChanged));
    m_peerConnection->onIceStateChange(makeWeakCallback(this, &WebRtcCtl::onPeerIceStateChanged));
    m_peerConnection->onGatheringStateChange(makeWeakCallback(this, &WebRtcCtl::onPeerGatheringStateChanged));
    m_peerConnection->onLocalDescription(makeWeakCallback(this, &WebRtcCtl::onPeerLocalDescription));
    m_peerConnection->onLocalCandidate(makeWeakCallback(this, &WebRtcCtl::onPeerLocalCandidate));

    /* 设置轨道回调 - 使用onMessage接收解包后的H264数据 */
    if (m_videoTrack)
    {
        LOG_INFO("Setting up video track message callback");
        m_videoTrack->onFrame(makeWeakCallback(this, &WebRtcCtl::onVideoFrameReceived));
        LOG_INFO("Video track message callback set");
    }

    if (m_audioTrack)
    {
        LOG_INFO("Setting up audio track message callback");
        m_audioTrack->onFrame(makeWeakCallback(this, &WebRtcCtl::onAudioFrameReceived));
        LOG_INFO("Audio track message callback set");
    }

    /* 接收到远程轨道回调（备用，以防某些情况下需要） */
    m_peerConnection->onTrack(makeWeakCallback(this, &WebRtcCtl::onRemoteTrack));

    /* 接收到数据通道回调 */
    m_peerConnection->onDataChannel(makeWeakCallback(this, &WebRtcCtl::onRemoteDataChannel));
}

void WebRtcCtl::onPeerConnectionStateChanged(rtc::PeerConnection::State state)
{
    m_connected = (state == rtc::PeerConnection::State::Connected);

    std::string stateStr;
    if (state == rtc::PeerConnection::State::Connected)
    {
        stateStr = "Connected";
        rtc::Candidate local;
        rtc::Candidate remote;
        if (m_peerConnection && m_peerConnection->getSelectedCandidatePair(&local, &remote))
        {
            LOG_INFO("Selected candidate pair: local={}, remote={}", std::string(local), std::string(remote));
            publishNetworkPathState(selectedNetworkPathFromPair(QString::fromStdString(std::string(local)),
                                                                QString::fromStdString(std::string(remote))));
        }
        stopReconnect();
    }
    else if (state == rtc::PeerConnection::State::Connecting)
        stateStr = "Checking";
    else if (state == rtc::PeerConnection::State::New)
        stateStr = "New";
    else if (state == rtc::PeerConnection::State::Failed)
    {
        stateStr = "Failed";
        scheduleReconnect();
    }
    else if (state == rtc::PeerConnection::State::Disconnected)
        stateStr = "Disconnected";
    else if (state == rtc::PeerConnection::State::Closed)
    {
        stateStr = "Closed";
        scheduleReconnect();
    }
    else
        stateStr = "Unknown";
    LOG_DEBUG("Control side connection state: {}", stateStr);
}

void WebRtcCtl::onPeerIceStateChanged(rtc::PeerConnection::IceState state)
{
    std::string stateStr;
    if (state == rtc::PeerConnection::IceState::Connected)
    {
        stateStr = "Connected";
        stopReconnect();
    }
    else if (state == rtc::PeerConnection::IceState::Checking)
        stateStr = "Checking";
    else if (state == rtc::PeerConnection::IceState::New)
        stateStr = "New";
    else if (state == rtc::PeerConnection::IceState::Failed)
    {
        stateStr = "Failed";
        scheduleReconnect();
    }
    else if (state == rtc::PeerConnection::IceState::Disconnected)
        stateStr = "Disconnected";
    else if (state == rtc::PeerConnection::IceState::Closed)
    {
        stateStr = "Closed";
        scheduleReconnect();
    }
    else if (state == rtc::PeerConnection::IceState::Completed)
    {
        stateStr = "Completed";
        stopReconnect();
    }
    else
        stateStr = "Unknown";
    LOG_INFO("Control side ICE state: {}", stateStr);
}

void WebRtcCtl::onPeerGatheringStateChanged(rtc::PeerConnection::GatheringState state)
{
    std::string stateStr;
    if (state == rtc::PeerConnection::GatheringState::InProgress)
        stateStr = "InProgress";
    else if (state == rtc::PeerConnection::GatheringState::Complete)
        stateStr = "Complete";
    else if (state == rtc::PeerConnection::GatheringState::New)
        stateStr = "New";
    else
        stateStr = "Unknown";
    LOG_INFO("Control side ICE gathering state: {}", stateStr);
}

void WebRtcCtl::onPeerLocalDescription(rtc::Description description)
{
    LOG_INFO("Control side local description set");
    try
    {
        QString sdp = SdpQualityPatcher::apply(QString::fromStdString(std::string(description)));
        QString type = QString::fromStdString(description.typeString());

        QJsonObject descriptionMsg = JsonUtil::createObject()
                                         .add(Constant::KEY_ROLE, Constant::ROLE_CTL)
                                         .add(Constant::KEY_TYPE, type)
                                         .add(Constant::KEY_RECEIVER, m_remoteId)
                                         .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                                         .add(Constant::KEY_DATA, sdp)
                                         .build();

        QString message = JsonUtil::toCompactString(descriptionMsg);
        emit sendWsCliTextMsg(message);
        LOG_INFO("Sent local description type={} to cli, size={} bytes", type, message.toUtf8().size());
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to send local description: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Failed to send local description: unknown error");
    }
}

void WebRtcCtl::onPeerLocalCandidate(const rtc::Candidate &candidate)
{
    QString candidateStr = QString::fromStdString(std::string(candidate));
    QString midStr = QString::fromStdString(candidate.mid());

    QJsonObject candidateMsg = JsonUtil::createObject()
                                   .add(Constant::KEY_ROLE, Constant::ROLE_CTL)
                                   .add(Constant::KEY_TYPE, Constant::TYPE_CANDIDATE)
                                   .add(Constant::KEY_RECEIVER, m_remoteId)
                                   .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                                   .add(Constant::KEY_DATA, candidateStr)
                                   .add(Constant::KEY_MID, midStr)
                                   .build();

    QString message = JsonUtil::toCompactString(candidateMsg);
    emit sendWsCliTextMsg(message);
    noteLocalNetworkCandidate(candidateStr);
    LOG_DEBUG("Sent local ICE candidate to cli, mid={}, size={} bytes", midStr, message.toUtf8().size());
}

void WebRtcCtl::onVideoFrameReceived(rtc::binary data, rtc::FrameInfo info)
{
    LOG_TRACE("Video frame received: {}, timestamp: {}", ConvertUtil::formatFileSize(data.size()), info.timestamp);
    enqueueVideoFrame(std::move(data), info);
}

void WebRtcCtl::onAudioFrameReceived(rtc::binary data, rtc::FrameInfo info)
{
    LOG_TRACE("Audio frame received: {}, ts: {}", ConvertUtil::formatFileSize(data.size()), info.timestamp);
    processAudioFrame(data, info);
}

void WebRtcCtl::onRemoteTrack(std::shared_ptr<rtc::Track> track)
{
    if (!track)
        return;

    QString trackMid = QString::fromStdString(track->mid());
    const QString trackType = QString::fromStdString(track->description().type());
    LOG_INFO("Control side received remote track: mid={}, type={}, direction={}", trackMid, trackType, static_cast<int>(track->direction()));

    if (trackType == QStringLiteral("video"))
    {
        if (m_videoTrack && m_videoTrack != track)
        {
            m_videoTrack->resetCallbacks();
            m_videoTrack->close();
        }
        m_videoTrack = track;
        auto h264Depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
        m_videoTrack->setMediaHandler(h264Depacketizer);
        m_videoTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
        m_videoTrack->onFrame(makeWeakCallback(this, &WebRtcCtl::onVideoFrameReceived));
        LOG_INFO("Remote video track media handlers installed for mid={}", trackMid);
        return;
    }

    if (trackType == QStringLiteral("audio"))
    {
        if (m_audioTrack && m_audioTrack != track)
        {
            m_audioTrack->resetCallbacks();
            m_audioTrack->close();
        }
        m_audioTrack = track;
        m_audioTrack->setMediaHandler(std::make_shared<OpusDuplexMediaHandler>(3, Constant::TYPE_AUDIO.toStdString(), 111));
        m_audioTrack->onFrame(makeWeakCallback(this, &WebRtcCtl::onAudioFrameReceived));
        LOG_INFO("Remote audio track callback installed for mid={}", trackMid);
    }
}

void WebRtcCtl::onRemoteDataChannel(std::shared_ptr<rtc::DataChannel> channel)
{
    QString channelLabel = QString::fromStdString(channel->label());
    LOG_INFO("Control side received data channel: {}", channelLabel);

    if (channelLabel == Constant::TYPE_FILE)
    {
        m_fileChannel = channel;
        setupFileChannelCallbacks();
    }
    else if (channelLabel == Constant::TYPE_FILE_TEXT)
    {
        m_fileTextChannel = channel;
        setupFileTextChannelCallbacks();
    }
    else if (channelLabel == "video_data")
    {
        m_videoDataChannel = channel;
        setupVideoDataChannelCallbacks();
    }
    else if (channelLabel == Constant::TYPE_INPUT)
    {
        m_inputChannel = channel;
        setupInputChannelCallbacks();
    }
}
