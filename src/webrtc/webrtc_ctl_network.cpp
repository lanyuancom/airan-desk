#include "webrtc_ctl.h"

#include "../codec/h264_decoder.h"
#include "../common/constant.h"
#include "../util/file_packet_util.h"
#include "../util/json_util.h"

#include <QDateTime>
#include <QTimer>
#include <QThread>
#include <algorithm>

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

QStringList orderedNetworkPaths(QStringList paths)
{
    paths.removeDuplicates();
    QStringList ordered;
    for (const QString &path : {QStringLiteral("auto"), QStringLiteral("direct"), QStringLiteral("turn_udp"), QStringLiteral("turn_tcp")})
    {
        if (paths.contains(path))
            ordered.append(path);
    }
    return ordered;
}
} /* namespace */

void WebRtcCtl::applyLocalStreamConfig(const QJsonObject &object)
{
    if (JsonUtil::getString(object, Constant::KEY_MSGTYPE) != Constant::TYPE_STREAM_CONFIG)
        return;

    bool streamResetRequired = false;
    const bool statusOnly = JsonUtil::getBool(object, QStringLiteral("statusOnly"), false);

    const QString mode = JsonUtil::getString(object, Constant::KEY_QUALITY).toLower();
    if (!statusOnly && (mode == QStringLiteral("quality") || mode == QStringLiteral("smooth") || mode == QStringLiteral("compat")))
    {
        if (m_streamMode != mode)
        {
            m_streamMode = mode;
            m_acceptReliableVideo.store(mode == QStringLiteral("quality") || mode == QStringLiteral("compat"));
            streamResetRequired = true;
            LOG_INFO("Control receive mode switched to {}", m_streamMode);
        }
    }

    const QString bitrateProfile = JsonUtil::getString(object, Constant::KEY_BITRATE_PROFILE).toLower();
    if (!statusOnly && (bitrateProfile == QStringLiteral("low") || bitrateProfile == QStringLiteral("medium") || bitrateProfile == QStringLiteral("high")))
    {
        if (m_bitrateProfile != bitrateProfile)
        {
            m_bitrateProfile = bitrateProfile;
            streamResetRequired = true;
            LOG_INFO("Control bitrate profile switched to {}", m_bitrateProfile);
        }
    }

    const QString networkPath = JsonUtil::getString(object, Constant::KEY_NETWORK_PATH).toLower();
    if (!statusOnly && (networkPath == QStringLiteral("auto") ||
        networkPath == QStringLiteral("direct") ||
        networkPath == QStringLiteral("turn_udp") ||
        networkPath == QStringLiteral("turn_tcp")))
    {
        m_networkPath = networkPath;
        publishNetworkPathState();
    }

    if (!statusOnly && (object.contains(Constant::KEY_WIDTH) || object.contains(Constant::KEY_HEIGHT)))
    {
        const int requestedWidth = JsonUtil::getInt(object, Constant::KEY_WIDTH, m_requestedWidth);
        const int requestedHeight = JsonUtil::getInt(object, Constant::KEY_HEIGHT, m_requestedHeight);
        if (requestedWidth != m_requestedWidth || requestedHeight != m_requestedHeight)
        {
            m_requestedWidth = requestedWidth;
            m_requestedHeight = requestedHeight;
            streamResetRequired = true;
            LOG_INFO("Control receive resolution switched to {}x{}", m_requestedWidth, m_requestedHeight);
        }
    }

    if (streamResetRequired)
    {
        resetVideoReceivePipeline("receive-stream-change", true);
        LOG_INFO("Control receive pipeline reset for stream change");
    }

    if (!statusOnly)
        Q_EMIT remoteStreamModeChanged(m_streamMode);

    const QString osName = JsonUtil::getString(object, Constant::KEY_OS).toLower();
    if (!osName.isEmpty())
    {
        Q_EMIT remoteOsChanged(osName);
        LOG_INFO("Remote OS reported: {}", osName);
    }

    QStringList backends;
    const QJsonArray backendArray = JsonUtil::getArray(object, Constant::KEY_CAPTURE_BACKENDS);
    for (const QJsonValue &value : backendArray)
    {
        const QString backend = value.toString();
        if (!backend.isEmpty() && !backends.contains(backend))
            backends.append(backend);
    }
    const QString captureBackend = JsonUtil::getString(object, Constant::KEY_CAPTURE_BACKEND);
    if (!backends.isEmpty() || !captureBackend.isEmpty())
    {
        Q_EMIT remoteCaptureBackendsChanged(backends, captureBackend);
        LOG_INFO("Remote capture backend status: current={}, available={}", captureBackend, backends.join(","));
    }

    const QString encoderName = JsonUtil::getString(object, "encoderName");
    const QString encoderType = JsonUtil::getString(object, "encoderType");
    const bool encoderZeroCopy = JsonUtil::getBool(object, "encoderZeroCopy");
    if (!encoderName.isEmpty() || !encoderType.isEmpty())
    {
        Q_EMIT remoteEncoderChanged(encoderName, encoderType, encoderZeroCopy);
        LOG_INFO("Remote encoder status: encoder={}, type={}, zeroCopy={}", encoderName, encoderType, encoderZeroCopy);
    }
}

void WebRtcCtl::noteLocalNetworkCandidate(const QString &candidate)
{
    const QString path = networkPathFromCandidate(candidate);
    if (path.isEmpty())
        return;

    if (!m_availableNetworkPaths.contains(path))
        m_availableNetworkPaths.append(path);
    publishNetworkPathState();
}

void WebRtcCtl::publishNetworkPathState(const QString &selectedPath)
{
    if (!selectedPath.isEmpty())
    {
        m_selectedNetworkPath = selectedPath;
        if (!m_availableNetworkPaths.contains(selectedPath))
            m_availableNetworkPaths.append(selectedPath);
    }

    Q_EMIT networkPathStateChanged(orderedNetworkPaths(m_availableNetworkPaths),
                                   m_selectedNetworkPath,
                                   m_networkPath);
}

void WebRtcCtl::inputChannelSendMsg(const rtc::message_variant &data)
{
    LOG_TRACE("inputChannelSendMsg called - connected: {}, inputChannel: {}, isOpen: {}",
              m_connected,
              (m_inputChannel != nullptr),
              (m_inputChannel && m_inputChannel->isOpen()));

    if (m_inputChannel && m_inputChannel->isOpen())
    {
        try
        {
            bool isMouseMove = false;
            if (std::holds_alternative<std::string>(data))
            {
                const std::string &msg = std::get<std::string>(data);
                if (msg.find("\"msgType\":\"stream_config\"") != std::string::npos)
                {
                    applyLocalStreamConfig(JsonUtil::safeParseObject(QByteArray::fromStdString(msg)));
                }
                isMouseMove = (msg.find("\"msgType\":\"mouse\"") != std::string::npos &&
                               msg.find("\"dwFlags\":\"move\"") != std::string::npos);
            }

            /* 弱网保护：鼠标移动事件限流（约 60fps） */
            if (isMouseMove)
            {
                qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                if ((nowMs - m_lastInputMoveSendMs) < 16)
                    return;
                m_lastInputMoveSendMs = nowMs;

                /* 若发送缓冲区过高，直接丢弃旧 move 事件，避免输入“卡死感” */
                size_t buffered = m_inputChannel->bufferedAmount();
                if (buffered > 64 * 1024)
                {
                    LOG_TRACE("Drop mouse move due to channel backlog: {} bytes", buffered);
                    return;
                }
            }

            if (!m_inputChannel->send(data))
            {
                LOG_TRACE("Input message buffered by SCTP");
            }
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Failed to send input channel message: {}", e.what());
        }
        catch (...)
        {
            LOG_ERROR("Failed to send input channel message: unknown error");
        }
    }
    else
    {
        /* 若输入通道不可用，尝试触发恢复；重复调用由 scheduleReconnect 内部去重 */
        scheduleReconnect();

        /* 2秒节流一次告警，避免鼠标移动导致日志风暴 */
        qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - m_lastInputNotReadyLogMs >= 2000)
        {
            m_lastInputNotReadyLogMs = nowMs;
            LOG_WARN("Input channel not ready for sending - channel exists: {}, channel open: {}",
                     (m_inputChannel != nullptr),
                     (m_inputChannel && m_inputChannel->isOpen()));
        }
    }
}

void WebRtcCtl::sendControlHeartbeat()
{
    if (m_isOnlyFile)
        return;

    if (!m_inputChannel || !m_inputChannel->isOpen())
    {
        if (m_controlHeartbeatTimer)
            m_controlHeartbeatTimer->stop();
        return;
    }

    int decodeQueue = 0;
    {
        QMutexLocker locker(&m_videoQueueMutex);
        decodeQueue = m_videoFrameQueue.size();
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool recentPacketAfterDecode = m_lastVideoDecodedMs > 0 &&
                                         m_lastVideoPacketMs > m_lastVideoDecodedMs &&
                                         nowMs - m_lastVideoPacketMs < 2500;
    const bool recentStartupPacket = m_firstVideoPacketWithoutDecodeMs > 0 &&
                                     m_lastVideoPacketMs >= m_firstVideoPacketWithoutDecodeMs &&
                                     nowMs - m_lastVideoPacketMs < 2500;
    if (recentPacketAfterDecode && nowMs - m_lastVideoDecodedMs > 3000 && nowMs - m_lastVideoWatchdogMs > 3000)
    {
        m_lastVideoWatchdogMs = nowMs;
        LOG_WARN("Video watchdog detected decode stall: lastDecoded={}ms ago, queue={}", nowMs - m_lastVideoDecodedMs, decodeQueue);
        maybeSendVideoAdaptFeedback(true, true, "watchdog-stall");
        resetVideoReceivePipeline("watchdog-stall", true);
    }
    else if (m_lastVideoDecodedMs <= 0 && recentStartupPacket && nowMs - m_firstVideoPacketWithoutDecodeMs > 3000 && nowMs - m_lastVideoWatchdogMs > 3000)
    {
        m_lastVideoWatchdogMs = nowMs;
        LOG_WARN("Video watchdog detected startup decode stall: firstPacketWithoutDecode={}ms ago, queue={}", nowMs - m_firstVideoPacketWithoutDecodeMs, decodeQueue);
        maybeSendVideoAdaptFeedback(true, true, "watchdog-startup");
        resetVideoReceivePipeline("watchdog-startup", true);
    }

    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_CONTROL_HEARTBEAT)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, m_remoteId)
                          .add(Constant::KEY_RECEIVER_PWD, m_remotePwdMd5)
                          .build();
    try
    {
        m_inputChannel->send(rtc::message_variant(JsonUtil::toCompactBytes(obj).toStdString()));
    }
    catch (const std::exception &e)
    {
        LOG_WARN("Failed to send control heartbeat: {}", e.what());
    }
}

void WebRtcCtl::maybeSendVideoAdaptFeedback(bool congested, bool stalled, const char *reason)
{
    if (m_isOnlyFile || !m_inputChannel || !m_inputChannel->isOpen())
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool recentDecodeStall = m_lastVideoDecodedMs > 0 &&
                                   m_lastVideoPacketMs > m_lastVideoDecodedMs &&
                                   nowMs - m_lastVideoPacketMs < 2500 &&
                                   nowMs - m_lastVideoDecodedMs > 2500;
    congested = congested || m_videoFeedbackDroppedFrames > 2 || m_videoFeedbackKeyframeRequests > 0;
    stalled = stalled || recentDecodeStall;

    if (congested || stalled)
    {
        if (nowMs - m_lastVideoAdaptFeedbackMs < 4000)
            return;
    }
    else
    {
        if (nowMs - m_lastVideoAdaptFeedbackMs < 12000)
            return;
    }

    int decodeQueue = 0;
    {
        QMutexLocker locker(&m_videoQueueMutex);
        decodeQueue = m_videoFrameQueue.size();
    }
    const qint64 arrivalSpanMs = m_videoArrivalWindowStartMs > 0 ? qMax<qint64>(1, nowMs - m_videoArrivalWindowStartMs) : 0;
    const qint64 arrivalBitrateKbps = arrivalSpanMs > 0 ? (m_videoArrivalBytes * 8 / arrivalSpanMs) : 0;
    const int arrivalFrames = m_videoArrivalFrames;
    const qint64 arrivalBytes = m_videoArrivalBytes;
    const int interArrivalAvgMs = qRound(m_videoArrivalAvgGapMs);
    const int interArrivalJitterMs = qRound(m_videoArrivalJitterMs);
    const quint64 latestFrameTimestampUs = m_videoArrivalLastTimestampUs;
    m_lastRuntimeDiagnostics = QStringList{
        tr("Receiver: connected=%1 mode=%2 audio=%3")
            .arg(m_connected)
            .arg(m_streamMode, m_audioMode),
        tr("Video: decodedDelta=%1 droppedDelta=%2 pliDelta=%3 queue=%4 stalled=%5 congested=%6 reason=%7")
            .arg(m_videoFeedbackDecodedFrames)
            .arg(m_videoFeedbackDroppedFrames)
            .arg(m_videoFeedbackKeyframeRequests)
            .arg(decodeQueue)
            .arg(stalled)
            .arg(congested)
            .arg(reason ? QString::fromUtf8(reason) : QString()),
        tr("Arrival: frames=%1 bytes=%2 spanMs=%3 bitrateKbps=%4 avgGapMs=%5 jitterMs=%6 latestTsUs=%7")
            .arg(arrivalFrames)
            .arg(arrivalBytes)
            .arg(arrivalSpanMs)
            .arg(arrivalBitrateKbps)
            .arg(interArrivalAvgMs)
            .arg(interArrivalJitterMs)
            .arg(latestFrameTimestampUs),
        tr("Stream: mode=%1 bitrate=%2 network=%3 requested=%4x%5")
            .arg(m_streamMode, m_bitrateProfile, m_networkPath)
            .arg(m_requestedWidth)
            .arg(m_requestedHeight)}
                                   .join(QStringLiteral("\n"));
    Q_EMIT runtimeDiagnosticsUpdated(m_lastRuntimeDiagnostics);

    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_VIDEO_ADAPT_FEEDBACK)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, m_remoteId)
                          .add(Constant::KEY_RECEIVER_PWD, m_remotePwdMd5)
                          .add("congested", congested)
                          .add("stalled", stalled)
                          .add("receivingPackets", true)
                          .add("decodingFrames", m_videoFeedbackDecodedFrames > 0)
                          .add("deltaFramesDecoded", m_videoFeedbackDecodedFrames)
                          .add("deltaFramesDropped", m_videoFeedbackDroppedFrames)
                          .add("deltaPli", m_videoFeedbackKeyframeRequests)
                          .add("decodeQueue", decodeQueue)
                          .add("arrivalFrames", arrivalFrames)
                          .add("arrivalBytes", arrivalBytes)
                          .add("arrivalSpanMs", arrivalSpanMs)
                          .add("arrivalBitrateKbps", arrivalBitrateKbps)
                          .add("interArrivalAvgMs", interArrivalAvgMs)
                          .add("interArrivalJitterMs", interArrivalJitterMs)
                          .add("latestFrameTimestampUs", static_cast<double>(latestFrameTimestampUs))
                          .add("reason", reason ? QString::fromUtf8(reason) : QString())
                          .build();

    try
    {
        if (!m_inputChannel->send(rtc::message_variant(JsonUtil::toCompactBytes(obj).toStdString())))
            LOG_TRACE("Video adapt feedback buffered by SCTP");
        m_lastVideoAdaptFeedbackMs = nowMs;
        m_videoFeedbackDecodedFrames = 0;
        m_videoFeedbackDroppedFrames = 0;
        m_videoFeedbackKeyframeRequests = 0;
        m_videoArrivalWindowStartMs = nowMs;
        m_videoArrivalBytes = 0;
        m_videoArrivalFrames = 0;
    }
    catch (const std::exception &e)
    {
        LOG_WARN("Failed to send video adapt feedback: {}", e.what());
    }
}

void WebRtcCtl::noteVideoFrameArrival(qint64 nowMs, qint64 bytes, quint64 timestampUs)
{
    if (m_videoArrivalWindowStartMs <= 0)
        m_videoArrivalWindowStartMs = nowMs;

    if (m_videoArrivalLastMs > 0)
    {
        const qint64 gapMs = qBound<qint64>(1, nowMs - m_videoArrivalLastMs, 2000);
        if (m_videoArrivalAvgGapMs <= 0.0)
        {
            m_videoArrivalAvgGapMs = static_cast<double>(gapMs);
            m_videoArrivalJitterMs = 0.0;
        }
        else
        {
            const double diff = std::abs(static_cast<double>(gapMs) - m_videoArrivalAvgGapMs);
            m_videoArrivalAvgGapMs = m_videoArrivalAvgGapMs * 0.85 + static_cast<double>(gapMs) * 0.15;
            m_videoArrivalJitterMs = m_videoArrivalJitterMs * 0.85 + diff * 0.15;
        }
    }

    m_videoArrivalLastMs = nowMs;
    m_videoArrivalBytes += qMax<qint64>(0, static_cast<qint64>(bytes));
    ++m_videoArrivalFrames;
    if (timestampUs > 0)
        m_videoArrivalLastTimestampUs = timestampUs;
}

void WebRtcCtl::fileChannelSendMsg(const rtc::message_variant &data)
{
    LOG_TRACE("fileChannelSendMsg called - connected: {}, fileChannel: {}, isOpen: {}",
              m_connected,
              (m_fileChannel != nullptr),
              (m_fileChannel && m_fileChannel->isOpen()));

    if (m_connected && m_fileChannel && m_fileChannel->isOpen())
    {
        try
        {
            m_fileChannel->send(data);
            LOG_TRACE("Successfully sent file channel message");
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Failed to send file channel message: {}", e.what());
        }
        catch (...)
        {
            LOG_ERROR("Failed to send file channel message: unknown error");
        }
    }
    else
    {
        LOG_WARN("File channel not ready for sending - connected: {}, channel exists: {}, channel open: {}",
                 m_connected,
                 (m_fileChannel != nullptr),
                 (m_fileChannel && m_fileChannel->isOpen()));
    }
}

void WebRtcCtl::fileTextChannelSendMsg(const rtc::message_variant &data)
{
    LOG_TRACE("fileTextChannelSendMsg called - connected: {}, fileTextChannel: {}, isOpen: {}",
              m_connected,
              (m_fileTextChannel != nullptr),
              (m_fileTextChannel && m_fileTextChannel->isOpen()));

    if (m_connected && m_fileTextChannel && m_fileTextChannel->isOpen())
    {
        try
        {
            m_fileTextChannel->send(data);
            LOG_TRACE("Successfully sent file text channel message");
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Failed to send file text channel message: {}", e.what());
        }
        catch (...)
        {
            LOG_ERROR("Failed to send file text channel message: unknown error");
        }
    }
    else
    {
        LOG_WARN("File text channel not ready for sending - connected: {}, channel exists: {}, channel open: {}",
                 m_connected,
                 (m_fileTextChannel != nullptr),
                 (m_fileTextChannel && m_fileTextChannel->isOpen()));
    }
}
