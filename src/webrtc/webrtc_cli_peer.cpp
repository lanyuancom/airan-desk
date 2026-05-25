#include "webrtc_cli.h"

#include "opus_duplex_media_handler.h"
#include "sdp_quality_patcher.h"
#include "../common/constant.h"
#include "../codec/h264_profile_level.h"
#include "../desktop/desktop_capture_manager.h"
#include "../util/json_util.h"
#include "../util/qt_callback_util.h"

#include <QSet>

namespace
{
const QString kOpusFmtpHighQuality = QStringLiteral("minptime=10;useinbandfec=1;usedtx=0;stereo=1;sprop-stereo=1;maxplaybackrate=48000;maxaveragebitrate=48000");

QString sdpPayloadType(const QString &line, const QString &prefix)
{
    if (!line.startsWith(prefix))
        return QString();

    const int start = prefix.size();
    int end = line.indexOf(' ', start);
    if (end < 0)
        end = line.size();
    return line.mid(start, end - start).trimmed();
}

QString removeFmtpParam(const QString &params, const QString &key)
{
    const QStringList parts = params.split(QChar(';'));
    QStringList kept;
    const QString lowerKey = key.toLower();
    for (const QString &part : parts)
    {
        const QString item = part.trimmed();
        if (item.isEmpty() || item.toLower().startsWith(lowerKey + QStringLiteral("=")))
            continue;
        kept.append(item);
    }
    return kept.join(QStringLiteral(";"));
}

QString patchOpusFmtpLine(const QString &line)
{
    const int space = line.indexOf(QChar(' '));
    if (space < 0)
        return line + QLatin1Char(' ') + kOpusFmtpHighQuality;

    QString params = line.mid(space + 1);
    params = removeFmtpParam(params, QStringLiteral("minptime"));
    params = removeFmtpParam(params, QStringLiteral("useinbandfec"));
    params = removeFmtpParam(params, QStringLiteral("usedtx"));
    params = removeFmtpParam(params, QStringLiteral("stereo"));
    params = removeFmtpParam(params, QStringLiteral("sprop-stereo"));
    params = removeFmtpParam(params, QStringLiteral("maxplaybackrate"));
    params = removeFmtpParam(params, QStringLiteral("maxaveragebitrate"));

    const QString prefix = line.left(space + 1);
    return params.isEmpty() ? prefix + kOpusFmtpHighQuality : prefix + params + QLatin1Char(';') + kOpusFmtpHighQuality;
}

QStringList patchOpusAudioSection(const QStringList &section, bool &changed)
{
    QStringList opusPayloads;
    QSet<QString> fmtpPayloads;
    for (const QString &line : section)
    {
        const QString lower = line.toLower();
        if (lower.startsWith(QStringLiteral("a=rtpmap:")) && lower.contains(QStringLiteral(" opus/48000")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=rtpmap:"));
            if (!pt.isEmpty() && !opusPayloads.contains(pt))
                opusPayloads.append(pt);
        }
        else if (lower.startsWith(QStringLiteral("a=fmtp:")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=fmtp:"));
            if (!pt.isEmpty())
                fmtpPayloads.insert(pt);
        }
    }

    if (opusPayloads.isEmpty())
        return section;

    QStringList out;
    for (const QString &line : section)
    {
        const QString lower = line.toLower();
        if (lower.startsWith(QStringLiteral("a=fmtp:")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=fmtp:"));
            if (!pt.isEmpty() && opusPayloads.contains(pt))
            {
                const QString patched = patchOpusFmtpLine(line);
                out.append(patched);
                changed |= patched != line;
                continue;
            }
        }

        out.append(line);
        if (lower.startsWith(QStringLiteral("a=rtpmap:")) && lower.contains(QStringLiteral(" opus/48000")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=rtpmap:"));
            if (!pt.isEmpty() && !fmtpPayloads.contains(pt))
            {
                out.append(QStringLiteral("a=fmtp:%1 %2").arg(pt, kOpusFmtpHighQuality));
                changed = true;
            }
        }
    }
    return out;
}

QStringList patchH264VideoSection(const QStringList &section, const QString &profileLevelId, bool &changed)
{
    QStringList h264Payloads;
    QSet<QString> patchedPayloads;
    for (const QString &line : section)
    {
        const QString lower = line.toLower();
        if (lower.startsWith(QStringLiteral("a=rtpmap:")) && lower.contains(QStringLiteral(" h264/90000")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=rtpmap:"));
            if (!pt.isEmpty() && !h264Payloads.contains(pt))
                h264Payloads.append(pt);
        }
        else if (lower.startsWith(QStringLiteral("a=fmtp:")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=fmtp:"));
            if (!pt.isEmpty() && h264Payloads.contains(pt))
                patchedPayloads.insert(pt);
        }
    }

    if (h264Payloads.isEmpty())
        return section;

    QStringList out;
    for (const QString &line : section)
    {
        const QString lower = line.toLower();
        if (lower.startsWith(QStringLiteral("a=fmtp:")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=fmtp:"));
            if (!pt.isEmpty() && h264Payloads.contains(pt))
            {
                out.append(QStringLiteral("a=fmtp:%1 profile-level-id=%2;level-asymmetry-allowed=1;packetization-mode=1")
                               .arg(pt, profileLevelId));
                changed = true;
                continue;
            }
        }

        out.append(line);
        if (lower.startsWith(QStringLiteral("a=rtpmap:")) && lower.contains(QStringLiteral(" h264/90000")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=rtpmap:"));
            if (!pt.isEmpty() && !patchedPayloads.contains(pt))
            {
                out.append(QStringLiteral("a=fmtp:%1 profile-level-id=%2;level-asymmetry-allowed=1;packetization-mode=1")
                               .arg(pt, profileLevelId));
                changed = true;
            }
        }
    }
    return out;
}

QString ensureH264PacketizationMode(QString sdp, int width, int height, int fps)
{
    QString normalized = sdp;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QChar('\r'), QChar('\n'));

    const QString profileLevelId = H264ProfileLevel::constrainedBaselineProfileLevelId(width, height, fps);
    const QStringList lines = normalized.split(QChar('\n'));
    QStringList out;
    bool changed = false;
    int i = 0;
    while (i < lines.size())
    {
        const QString line = lines.at(i);
        if (line.startsWith(QStringLiteral("m=")))
        {
            QStringList section;
            section.append(line);
            ++i;
            while (i < lines.size() && !lines.at(i).startsWith(QStringLiteral("m=")))
            {
                section.append(lines.at(i));
                ++i;
            }

            if (line.startsWith(QStringLiteral("m=video")))
                out.append(patchH264VideoSection(section, profileLevelId, changed));
            else if (line.startsWith(QStringLiteral("m=audio")))
                out.append(patchOpusAudioSection(section, changed));
            else
                out.append(section);
            continue;
        }

        out.append(line);
        ++i;
    }

    return changed ? out.join(QStringLiteral("\r\n")) : sdp;
}
} /* namespace */

/* WebRTC核心功能 */
void WebRtcCli::initPeerConnection()
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
        LOG_ERROR("Unknown error during PeerConnection initialization");
    }
}

void WebRtcCli::createTracksAndChannels()
{
    if (!m_peerConnection)
    {
        LOG_ERROR("PeerConnection not available for creating tracks");
        return;
    }

    try
    {
        if (!m_isOnlyFile)
        {
            /* 创建视频轨道 - 严格按照官方示例配置 */
            LOG_INFO("Creating video track");
            std::string video_name = Constant::TYPE_VIDEO.toStdString();
            rtc::Description::Video videoDesc(video_name); /* 使用固定流名称匹配接收端 */
            videoDesc.addH264Codec(96);                    /* H264 payload type */

            /* 设置SSRC和媒体流标识 - 关键配置 */
            uint32_t videoSSRC = 1;
            std::string msid = Constant::TYPE_VIDEO_MSID.toStdString();
            videoDesc.addSSRC(videoSSRC, video_name, msid, video_name);
            videoDesc.setDirection(rtc::Description::Direction::SendOnly);
            m_videoTrack = m_peerConnection->addTrack(videoDesc);

            /* 为视频轨道设置RTP打包器链 */
            auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(videoSSRC, video_name, 96, rtc::H264RtpPacketizer::ClockRate);
            /* FFmpeg/hardware encoders may mix 3-byte and 4-byte Annex-B start codes. */
            /* Accept both so Android receives complete SPS/PPS/IDR NAL units. */
            auto h264Packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtpConfig);

            /* 添加RTCP SR报告器 */
            auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
            h264Packetizer->addToChain(srReporter);

            /* 添加RTCP NACK响应器 */
            auto nackResponder = std::make_shared<rtc::RtcpNackResponder>(2048);
            h264Packetizer->addToChain(nackResponder);
            auto pliHandler = std::make_shared<rtc::PliHandler>(makeWeakCallback(this, &WebRtcCli::onVideoPliRequested));
            h264Packetizer->addToChain(pliHandler);

            const int encodePixels = qMax(1, m_encode_width * m_encode_height);
            const double pacingBps = encodePixels > 1920 * 1080 ? 24000000.0
                                      : encodePixels > 1280 * 720 ? 16000000.0
                                                                  : 10000000.0;
            auto pacingHandler = std::make_shared<rtc::PacingHandler>(pacingBps, std::chrono::milliseconds(5));
            h264Packetizer->addToChain(pacingHandler);
            LOG_INFO("Video RTP pacing enabled: {} bps, interval=5ms", static_cast<int>(pacingBps));

            m_videoTrack->setMediaHandler(h264Packetizer);

            /* 创建音频轨道 */
            LOG_INFO("Creating audio track");
            rtc::Description::Audio audioDesc(Constant::TYPE_AUDIO.toStdString()); /* 使用固定流名称匹配接收端 */
            audioDesc.addOpusCodec(111);                                           /* Opus payload type */

            /* 设置SSRC和媒体流标识 */
            uint32_t audioSSRC = 2;
            audioDesc.addSSRC(audioSSRC, Constant::TYPE_AUDIO.toStdString(), msid, Constant::TYPE_AUDIO.toStdString());
            audioDesc.setDirection(rtc::Description::Direction::SendRecv);
            m_audioTrack = m_peerConnection->addTrack(audioDesc);
            m_audioTrack->setMediaHandler(std::make_shared<OpusDuplexMediaHandler>(audioSSRC, Constant::TYPE_AUDIO.toStdString(), 111));
            m_audioTrack->onFrame(makeWeakCallback(this, &WebRtcCli::onAudioFrameReceived));

            /* 创建输入数据通道（低延迟优先：无序 + 最多0次重传） */
            LOG_INFO("Creating input data channel");
            rtc::Reliability inputReliability;
            inputReliability.unordered = true;
            inputReliability.maxRetransmits = 0;
            m_inputChannel = m_peerConnection->createDataChannel(Constant::TYPE_INPUT.toStdString(), {inputReliability});
            setupInputChannelCallbacks();

            if (m_qualityFirstMode)
            {
                LOG_INFO("Creating reliable video data channel");
                rtc::Reliability videoReliability = createVideoDataReliability();
                m_videoDataChannel = m_peerConnection->createDataChannel("video_data", {videoReliability});
                setupVideoDataChannelCallbacks();
            }
            else
            {
                LOG_INFO("Reliable video data channel skipped; desktop video uses RTP for mode={}", m_streamMode);
            }

        }
        /* 创建文件数据通道（用于二进制文件传输） */
        LOG_INFO("Creating file data channel");
        m_fileChannel = m_peerConnection->createDataChannel(Constant::TYPE_FILE.toStdString());
        setupFileChannelCallbacks();

        /* 创建文件文本数据通道（用于文件列表、目录切换等文本消息） */
        LOG_INFO("Creating file text data channel");
        m_fileTextChannel = m_peerConnection->createDataChannel(Constant::TYPE_FILE_TEXT.toStdString());
        setupFileTextChannelCallbacks();

        m_channelsReady = true;
        LOG_INFO("All tracks and channels created successfully");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to create tracks and channels: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Unknown error during tracks and channels creation");
    }
}

void WebRtcCli::setupCallbacks()
{
    if (!m_peerConnection)
        return;

    m_peerConnection->onStateChange(makeWeakCallback(this, &WebRtcCli::onPeerConnectionStateChanged));
    m_peerConnection->onIceStateChange(makeWeakCallback(this, &WebRtcCli::onPeerIceStateChanged));
    m_peerConnection->onGatheringStateChange(makeWeakCallback(this, &WebRtcCli::onPeerGatheringStateChanged));
    m_peerConnection->onLocalDescription(makeWeakCallback(this, &WebRtcCli::onPeerLocalDescription));
    m_peerConnection->onLocalCandidate(makeWeakCallback(this, &WebRtcCli::onPeerLocalCandidate));
}

void WebRtcCli::onPeerConnectionStateChanged(rtc::PeerConnection::State state)
{
    if (m_destroying)
    {
        LOG_DEBUG("Ignoring state change callback during destruction");
        return;
    }

    m_connected = (state == rtc::PeerConnection::State::Connected);
    std::string stateStr;
    if (m_connected)
    {
        stateStr = "Connected";
        rtc::Candidate local;
        rtc::Candidate remote;
        if (m_peerConnection && m_peerConnection->getSelectedCandidatePair(&local, &remote))
            LOG_INFO("Selected candidate pair: local={}, remote={}", std::string(local), std::string(remote));
    }
    else if (state == rtc::PeerConnection::State::Connecting)
        stateStr = "Checking";
    else if (state == rtc::PeerConnection::State::New)
        stateStr = "New";
    else if (state == rtc::PeerConnection::State::Failed)
        stateStr = "Failed";
    else if (state == rtc::PeerConnection::State::Disconnected)
        stateStr = "Disconnected";
    else if (state == rtc::PeerConnection::State::Closed)
        stateStr = "Closed";
    else
        stateStr = "Unknown";

    LOG_INFO("Client side connection state: {}", stateStr);
    if (m_isOnlyFile)
        return;

    if (m_connected)
    {
        LOG_INFO("WebRTC connection established, starting media capture");
        if (m_disconnectGraceTimer)
            QMetaObject::invokeMethod(m_disconnectGraceTimer, "stop", Qt::QueuedConnection);
        m_lastControlAliveMs = QDateTime::currentMSecsSinceEpoch();
        if (m_controlWatchdogTimer)
            QMetaObject::invokeMethod(m_controlWatchdogTimer, "start", Qt::QueuedConnection, Q_ARG(int, 2000));
        QMetaObject::invokeMethod(this, "startMediaCapture", Qt::QueuedConnection);
    }
    else if (state == rtc::PeerConnection::State::Disconnected)
    {
        LOG_INFO("WebRTC disconnected, starting grace timer before stopping media capture");
        if (m_controlWatchdogTimer)
            QMetaObject::invokeMethod(m_controlWatchdogTimer, "stop", Qt::QueuedConnection);
        if (m_disconnectGraceTimer)
            QMetaObject::invokeMethod(m_disconnectGraceTimer, "start", Qt::QueuedConnection, Q_ARG(int, 6000));
    }
    else if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed)
    {
        LOG_INFO("WebRTC connection failed/closed, stopping media capture");
        if (m_controlWatchdogTimer)
            QMetaObject::invokeMethod(m_controlWatchdogTimer, "stop", Qt::QueuedConnection);
        if (m_disconnectGraceTimer)
            QMetaObject::invokeMethod(m_disconnectGraceTimer, "stop", Qt::QueuedConnection);
        QMetaObject::invokeMethod(this, "stopMediaCapture", Qt::QueuedConnection);
    }
}

void WebRtcCli::onPeerIceStateChanged(rtc::PeerConnection::IceState state)
{
    std::string stateStr;
    if (state == rtc::PeerConnection::IceState::Connected)
        stateStr = "Connected";
    else if (state == rtc::PeerConnection::IceState::Checking)
        stateStr = "Checking";
    else if (state == rtc::PeerConnection::IceState::New)
        stateStr = "New";
    else if (state == rtc::PeerConnection::IceState::Failed)
        stateStr = "Failed";
    else if (state == rtc::PeerConnection::IceState::Disconnected)
        stateStr = "Disconnected";
    else if (state == rtc::PeerConnection::IceState::Closed)
        stateStr = "Closed";
    else if (state == rtc::PeerConnection::IceState::Completed)
        stateStr = "Completed";
    else
        stateStr = "Unknown";
    LOG_INFO("Client side ICE state: {}", stateStr);
}

void WebRtcCli::onPeerGatheringStateChanged(rtc::PeerConnection::GatheringState state)
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
    LOG_DEBUG("Client side gathering state: {}", stateStr);
}

void WebRtcCli::onPeerLocalDescription(rtc::Description description)
{
    try
    {
        QString sdp = SdpQualityPatcher::apply(QString::fromStdString(std::string(description)),
                                               m_encode_width, m_encode_height, m_fps);
        QString type = QString::fromStdString(description.typeString());

        QJsonObject descriptionMsg = JsonUtil::createObject()
                                         .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                                         .add(Constant::KEY_TYPE, type)
                                         .add(Constant::KEY_RECEIVER, m_remoteId)
                                         .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                                         .add(Constant::KEY_DATA, sdp)
                                         .build();

        QString message = JsonUtil::toCompactString(descriptionMsg);
        emit sendWsCliTextMsg(message);
        LOG_INFO("Sent local description type={} to ctl, size={} bytes", type, message.toUtf8().size());
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to send local description: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Unknown error during local description handling");
    }
}

void WebRtcCli::onPeerLocalCandidate(const rtc::Candidate &candidate)
{
    QString candidateStr = QString::fromStdString(std::string(candidate));
    QString midStr = QString::fromStdString(candidate.mid());

    QJsonObject candidateMsg = JsonUtil::createObject()
                                   .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                                   .add(Constant::KEY_TYPE, Constant::TYPE_CANDIDATE)
                                   .add(Constant::KEY_RECEIVER, m_remoteId)
                                   .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                                   .add(Constant::KEY_DATA, candidateStr)
                                   .add(Constant::KEY_MID, midStr)
                                   .build();

    QString message = JsonUtil::toCompactString(candidateMsg);
    emit sendWsCliTextMsg(message);
    LOG_DEBUG("Sent local ICE candidate to ctl, mid={}, size={} bytes", midStr, message.toUtf8().size());
}

rtc::Configuration WebRtcCli::buildRtcConfiguration() const
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
    {
        config.iceServers.push_back(rtc::IceServer(m_host, m_port));
    }

    if ((path == QStringLiteral("turn_udp") || path == QStringLiteral("auto")) && hasTurnCreds)
    {
        config.iceServers.push_back(rtc::IceServer(m_host, m_port, m_username, m_password, rtc::IceServer::RelayType::TurnUdp));
    }

    if ((path == QStringLiteral("turn_tcp") || path == QStringLiteral("auto")) && hasTurnCreds)
    {
        config.iceServers.push_back(rtc::IceServer(m_host, m_port, m_username, m_password, rtc::IceServer::RelayType::TurnTcp));
        config.enableIceTcp = true;
    }

    if (forceRelay)
        config.iceTransportPolicy = rtc::TransportPolicy::Relay;

    return config;
}

void WebRtcCli::setRemoteDescription(const QString &data, const QString &type)
{
    if (!m_peerConnection)
        return;

    try
    {
        rtc::Description::Type descType;
        if (type == Constant::TYPE_OFFER)
        {
            descType = rtc::Description::Type::Offer;
        }
        else if (type == Constant::TYPE_ANSWER)
        {
            descType = rtc::Description::Type::Answer;
        }
        else
        {
            LOG_ERROR("Unknown description type: {}", type);
            return;
        }

        rtc::Description description(data.toStdString(), descType);
        m_peerConnection->setRemoteDescription(description);
        m_remoteDescriptionSet = true;
        flushPendingRemoteCandidates();
        if (type == Constant::TYPE_OFFER)
        {
            m_peerConnection->createAnswer();
        }

        LOG_INFO("Set remote description: {}", type);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to set remote description: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Failed to set remote description: unknown error");
    }
}

void WebRtcCli::addIceCandidate(const QString &candidate, const QString &mid)
{
    addRemoteCandidateOrQueue(candidate, mid);
}

void WebRtcCli::addRemoteCandidateOrQueue(const QString &candidate, const QString &mid)
{
    if (!m_peerConnection)
        return;

    if (!m_remoteDescriptionSet)
    {
        m_pendingRemoteCandidates.append(qMakePair(candidate, mid));
        LOG_DEBUG("Queued remote ICE candidate until remote description is set, pending={}", m_pendingRemoteCandidates.size());
        return;
    }

    try
    {
        rtc::Candidate rtcCandidate(candidate.toStdString(), mid.toStdString());
        m_peerConnection->addRemoteCandidate(rtcCandidate);
        LOG_TRACE("Added ICE candidate");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to add ICE candidate: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Failed to add ICE candidate: unknown error");
    }
}

void WebRtcCli::flushPendingRemoteCandidates()
{
    if (!m_peerConnection || !m_remoteDescriptionSet || m_pendingRemoteCandidates.isEmpty())
        return;

    const QVector<QPair<QString, QString>> pending = m_pendingRemoteCandidates;
    m_pendingRemoteCandidates.clear();
    for (const auto &candidate : pending)
    {
        addRemoteCandidateOrQueue(candidate.first, candidate.second);
    }
}
