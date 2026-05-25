#include "webrtc_ctl.h"

#include "../codec/h264_decoder.h"
#include "../common/constant.h"
#include "../util/convert_util.h"
#include "../util/json_util.h"
#include "../util/qt_callback_util.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QMetaObject>
#include <algorithm>

namespace
{
bool isSeqOlderOrEqual(quint32 seq, quint32 lastSeq)
{
    return static_cast<qint32>(seq - lastSeq) <= 0;
}

quint16 readBigEndian16(const rtc::binary &message, size_t pos)
{
    return (static_cast<quint16>(message[pos]) << 8) |
           static_cast<quint16>(message[pos + 1]);
}

quint32 readBigEndian32(const rtc::binary &message, size_t pos)
{
    return (static_cast<quint32>(message[pos]) << 24) |
           (static_cast<quint32>(message[pos + 1]) << 16) |
           (static_cast<quint32>(message[pos + 2]) << 8) |
           static_cast<quint32>(message[pos + 3]);
}

quint64 readBigEndian64(const rtc::binary &message, size_t pos)
{
    quint64 value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<quint8>(message[pos + i]);
    return value;
}

uint8_t byteValueAt(const rtc::binary &data, size_t index)
{
    return static_cast<uint8_t>(data[index]);
}

bool isIdrNal(uint8_t nalHeader)
{
    return (nalHeader & 0x1F) == 5;
}

bool seqLessThan(quint32 a, quint32 b)
{
    return static_cast<qint32>(a - b) < 0;
}
} /* namespace */

void WebRtcCtl::setupVideoDataChannelCallbacks()
{
    if (!m_videoDataChannel)
        return;

    m_videoDataChannel->onOpen(makeWeakCallback(this, &WebRtcCtl::onVideoDataChannelOpen));
    m_videoDataChannel->onClosed(makeWeakCallback(this, &WebRtcCtl::onVideoDataChannelClosed));
    m_videoDataChannel->onError(makeWeakCallback(this, &WebRtcCtl::onVideoDataChannelError));
    m_videoDataChannel->onMessage(makeWeakCallback(this, &WebRtcCtl::onVideoDataChannelMessage));
}

void WebRtcCtl::onVideoDataChannelOpen()
{
    const QString channelLabel = m_videoDataChannel ? QString::fromStdString(m_videoDataChannel->label()) : QString();
    LOG_INFO("Reliable video data channel opened: {}", channelLabel);
    {
        QMutexLocker locker(&m_videoReliableMutex);
        m_videoFragmentBuffer.clear();
        m_videoFragmentReceivedCount.clear();
        m_videoFragmentTimestampUs.clear();
        m_videoFragmentFirstSeenMs.clear();
        m_lastAcceptedReliableVideoSeq = 0;
        m_hasLastAcceptedReliableVideoSeq = false;
    }
}

void WebRtcCtl::onVideoDataChannelClosed()
{
    const QString channelLabel = m_videoDataChannel ? QString::fromStdString(m_videoDataChannel->label()) : QString();
    LOG_INFO("Reliable video data channel closed: {}", channelLabel);
}

void WebRtcCtl::onVideoDataChannelError(const std::string &error)
{
    const QString channelLabel = m_videoDataChannel ? QString::fromStdString(m_videoDataChannel->label()) : QString();
    LOG_ERROR("Reliable video data channel error ({}): {}", channelLabel, error);
}

void WebRtcCtl::onVideoDataChannelMessage(const rtc::message_variant &message)
{
    if (!m_acceptReliableVideo.load())
    {
        LOG_TRACE("Drop reliable video message in smooth-first mode");
        return;
    }
    if (std::holds_alternative<rtc::binary>(message))
        processReliableVideoMessage(std::get<rtc::binary>(message));
    else
        LOG_WARN("Reliable video data channel received non-binary message, ignored");
}

bool WebRtcCtl::processReliableVideoMessage(const rtc::binary &message)
{
    if (!m_acceptReliableVideo.load())
        return true;

    constexpr quint32 kMagic = 0x41524456; /* "ARDV" */
    constexpr quint16 kHeaderSize = 28;

    if (message.size() < kHeaderSize || readBigEndian32(message, 0) != kMagic)
    {
        rtc::FrameInfo info{std::chrono::duration<double>(0)};
        enqueueVideoFrame(message, info);
        return true;
    }

    const quint16 version = readBigEndian16(message, 4);
    const quint16 headerSize = readBigEndian16(message, 6);
    const quint32 seq = readBigEndian32(message, 8);
    const quint32 totalFragments = readBigEndian32(message, 12);
    const quint32 index = readBigEndian32(message, 16);
    const quint64 timestampUs = readBigEndian64(message, 20);

    if (version != 1 || headerSize < kHeaderSize || headerSize > message.size() || totalFragments == 0 || index >= totalFragments)
    {
        LOG_WARN("Invalid reliable video fragment header: version={}, header={}, seq={}, index={}, total={}",
                 version, headerSize, seq, index, totalFragments);
        return false;
    }

    rtc::binary payload(message.begin() + headerSize, message.end());

    {
        QMutexLocker locker(&m_videoReliableMutex);
        pruneReliableVideoFragmentsLocked(QDateTime::currentMSecsSinceEpoch());
        if (m_hasLastAcceptedReliableVideoSeq && isSeqOlderOrEqual(seq, m_lastAcceptedReliableVideoSeq))
        {
            LOG_TRACE("Drop stale reliable video fragment/frame: seq={}, lastAccepted={}", seq, m_lastAcceptedReliableVideoSeq);
            return true;
        }
    }

    if (totalFragments == 1)
    {
        rtc::FrameInfo info{std::chrono::duration<double, std::micro>(timestampUs)};
        if (enqueueVideoFrame(std::move(payload), info))
        {
            QMutexLocker locker(&m_videoReliableMutex);
            m_lastAcceptedReliableVideoSeq = seq;
            m_hasLastAcceptedReliableVideoSeq = true;
        }
        return true;
    }

    rtc::binary frame;
    {
        QMutexLocker locker(&m_videoReliableMutex);
        if (m_hasLastAcceptedReliableVideoSeq && isSeqOlderOrEqual(seq, m_lastAcceptedReliableVideoSeq))
            return true;

        auto &fragments = m_videoFragmentBuffer[seq];
        if (fragments.isEmpty())
        {
            fragments.resize(static_cast<int>(totalFragments));
            m_videoFragmentFirstSeenMs[seq] = QDateTime::currentMSecsSinceEpoch();
        }

        if (fragments.size() != static_cast<int>(totalFragments))
        {
            m_videoFragmentBuffer.remove(seq);
            m_videoFragmentReceivedCount.remove(seq);
            m_videoFragmentTimestampUs.remove(seq);
            m_videoFragmentFirstSeenMs.remove(seq);
            LOG_WARN("Reliable video fragment total mismatch, drop seq={}", seq);
            return false;
        }

        if (fragments[static_cast<int>(index)].empty())
            m_videoFragmentReceivedCount[seq] = m_videoFragmentReceivedCount.value(seq, 0) + 1;
        fragments[static_cast<int>(index)] = std::move(payload);
        m_videoFragmentTimestampUs[seq] = timestampUs;

        if (m_videoFragmentReceivedCount.value(seq, 0) < static_cast<int>(totalFragments))
            return true;

        size_t frameSize = 0;
        for (const auto &fragment : fragments)
            frameSize += fragment.size();
        frame.reserve(frameSize);
        for (const auto &fragment : fragments)
            frame.insert(frame.end(), fragment.begin(), fragment.end());

        m_videoFragmentBuffer.remove(seq);
        m_videoFragmentReceivedCount.remove(seq);
        m_videoFragmentTimestampUs.remove(seq);
        m_videoFragmentFirstSeenMs.remove(seq);
    }

    rtc::FrameInfo info{std::chrono::duration<double, std::micro>(timestampUs)};
    LOG_TRACE("Reliable video frame reassembled: seq={}, fragments={}, size={}",
              seq, totalFragments, ConvertUtil::formatFileSize(frame.size()));
    if (enqueueVideoFrame(std::move(frame), info))
    {
        QMutexLocker locker(&m_videoReliableMutex);
        m_lastAcceptedReliableVideoSeq = seq;
        m_hasLastAcceptedReliableVideoSeq = true;
    }
    return true;
}

bool WebRtcCtl::isH264Keyframe(const rtc::binary &data) const
{
    for (size_t i = 0; i + 3 < data.size(); ++i)
    {
        if (byteValueAt(data, i) != 0x00 || byteValueAt(data, i + 1) != 0x00)
            continue;

        size_t nalStart = std::string::npos;
        if (byteValueAt(data, i + 2) == 0x01)
            nalStart = i + 3;
        else if (i + 4 < data.size() && byteValueAt(data, i + 2) == 0x00 && byteValueAt(data, i + 3) == 0x01)
            nalStart = i + 4;

        if (nalStart != std::string::npos && nalStart < data.size() && isIdrNal(byteValueAt(data, nalStart)))
            return true;
    }

    const int avcc4 = scanLengthPrefixedH264(data, 4);
    if (avcc4 == 1)
        return true;
    if (avcc4 == 0)
        return false;

    if (scanLengthPrefixedH264(data, 2) == 1)
        return true;

    return false;
}

int WebRtcCtl::scanLengthPrefixedH264(const rtc::binary &data, size_t lengthSize) const
{
    size_t pos = 0;
    bool parsedAny = false;
    while (pos + lengthSize < data.size())
    {
        quint32 nalSize = 0;
        for (size_t i = 0; i < lengthSize; ++i)
            nalSize = (nalSize << 8) | byteValueAt(data, pos + i);

        if (nalSize == 0 || nalSize > data.size() - pos - lengthSize)
            return -1;

        const size_t nalStart = pos + lengthSize;
        if (isIdrNal(byteValueAt(data, nalStart)))
            return 1;

        parsedAny = true;
        pos = nalStart + nalSize;
    }
    return parsedAny && pos == data.size() ? 0 : -1;
}

bool WebRtcCtl::enqueueVideoFrame(rtc::binary videoData, const rtc::FrameInfo &frameInfo)
{
    if (videoData.empty())
        return false;

    m_lastVideoPacketMs = QDateTime::currentMSecsSinceEpoch();
    const quint64 timestampUs = frameInfo.timestampSeconds
                                    ? static_cast<quint64>(std::chrono::duration_cast<std::chrono::microseconds>(*frameInfo.timestampSeconds).count())
                                    : 0;
    noteVideoFrameArrival(m_lastVideoPacketMs, static_cast<qint64>(videoData.size()), timestampUs);
    if (m_lastVideoDecodedMs <= 0 && m_firstVideoPacketWithoutDecodeMs <= 0)
        m_firstVideoPacketWithoutDecodeMs = m_lastVideoPacketMs;
    const bool isKeyframe = isH264Keyframe(videoData);
    if (!shouldDecodeVideoFrame(isKeyframe, "enqueue"))
        return false;

    bool shouldSchedule = false;
    bool shouldRequestKeyframe = false;
    int queuedFrames = 0;
    {
        QMutexLocker locker(&m_videoQueueMutex);
        QueuedVideoFrame frame;
        frame.keyframe = isKeyframe;
        frame.info = frameInfo;

        frame.data = std::move(videoData);
        m_videoFrameQueue.enqueue(std::move(frame));
        queuedFrames = m_videoFrameQueue.size();

        shouldRequestKeyframe = pruneVideoFrameQueueLocked();

        if (!m_videoDrainScheduled)
        {
            m_videoDrainScheduled = true;
            shouldSchedule = true;
        }
    }

    if (shouldRequestKeyframe)
    {
        if (m_h264Decoder)
            m_h264Decoder->flushBuffers();
        requestRemoteKeyframe("waiting-for-idr");
    }
    if (queuedFrames > 3)
        maybeSendVideoAdaptFeedback(true, false, "decode-queue");

    if (shouldSchedule)
    {
        QMetaObject::invokeMethod(this, "drainVideoFrameQueue", Qt::QueuedConnection);
    }
    return true;
}

bool WebRtcCtl::pruneVideoFrameQueueLocked()
{
    constexpr int kMaxRealtimeQueueFrames = 3;
    bool shouldRequestKeyframe = false;

    if (m_videoFrameQueue.size() <= kMaxRealtimeQueueFrames)
        return false;

    int latestKeyframeIndex = -1;
    for (int i = m_videoFrameQueue.size() - 1; i >= 0; --i)
    {
        if (m_videoFrameQueue.at(i).keyframe)
        {
            latestKeyframeIndex = i;
            break;
        }
    }

    if (latestKeyframeIndex >= 0)
    {
        const int dropped = latestKeyframeIndex;
        for (int i = 0; i < dropped; ++i)
            m_videoFrameQueue.dequeue();
        if (dropped > 0)
        {
            m_videoFeedbackDroppedFrames += dropped;
            LOG_WARN("Video decode queue backlog, dropped {} stale frames and resume from latest IDR", dropped);
        }
        return false;
    }

    if (!m_waitingForVideoKeyframe.load())
    {
        const int dropped = m_videoFrameQueue.size() - kMaxRealtimeQueueFrames;
        for (int i = 0; i < dropped; ++i)
            m_videoFrameQueue.dequeue();
        if (dropped > 0)
        {
            m_videoFeedbackDroppedFrames += dropped;
            LOG_WARN("Video decode queue backlog, dropped {} old P frames to keep realtime latency", dropped);
        }
        return false;
    }

    /* 暂时没有新的 I 帧时，继续堆积旧 P 帧只会增加延迟；清空队列并请求 IDR。 */
    const int dropped = m_videoFrameQueue.size();
    m_videoFrameQueue.clear();
    if (dropped > 0)
    {
        m_videoFeedbackDroppedFrames += dropped;
        startWaitingForVideoKeyframe("queue-backlog");
        LOG_WARN("Video decode queue backlog while waiting for IDR, dropped {} stale P frames and requested IDR", dropped);
        shouldRequestKeyframe = true;
    }
    return shouldRequestKeyframe;
}

void WebRtcCtl::pruneReliableVideoFragmentsLocked(qint64 nowMs)
{
    constexpr qint64 kReliableVideoFragmentTimeoutMs = 10000;
    constexpr int kMaxReliableVideoFragmentFrames = 96;

    QList<quint32> expiredSeqs;
    for (auto it = m_videoFragmentFirstSeenMs.cbegin(); it != m_videoFragmentFirstSeenMs.cend(); ++it)
    {
        if (nowMs - it.value() > kReliableVideoFragmentTimeoutMs)
            expiredSeqs.append(it.key());
    }

    if (m_videoFragmentBuffer.size() > kMaxReliableVideoFragmentFrames)
    {
        QList<quint32> seqs = m_videoFragmentBuffer.keys();
        std::sort(seqs.begin(), seqs.end(), seqLessThan);
        const int extra = m_videoFragmentBuffer.size() - kMaxReliableVideoFragmentFrames;
        for (int i = 0; i < extra && i < seqs.size(); ++i)
            expiredSeqs.append(seqs.at(i));
    }

    for (quint32 seq : std::as_const(expiredSeqs))
    {
        m_videoFragmentBuffer.remove(seq);
        m_videoFragmentReceivedCount.remove(seq);
        m_videoFragmentTimestampUs.remove(seq);
        m_videoFragmentFirstSeenMs.remove(seq);
        LOG_TRACE("Drop expired reliable video fragments: seq={}", seq);
    }
}

bool WebRtcCtl::shouldDecodeVideoFrame(bool isKeyframe, const char *context)
{
    if (isKeyframe)
    {
        if (m_waitingForVideoKeyframe.load())
            LOG_INFO("Received H264 IDR while waiting for recovery, resume decoding ({})", context ? context : "unknown");
        m_waitingForVideoKeyframe.store(false);
        m_videoKeyframeWaitStartedMs = 0;
        m_keyframeRequestBackoffMs = 1000;
        return true;
    }

    if (!m_waitingForVideoKeyframe.load())
        return true;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 waitedMs = m_videoKeyframeWaitStartedMs > 0 ? nowMs - m_videoKeyframeWaitStartedMs : 0;
    requestRemoteKeyframe("waiting-for-idr");
    if (waitedMs > 2500)
    {
        LOG_WARN("Still waiting for H264 IDR after {}ms; probe-decode next non-IDR frame ({})",
                 waitedMs, context ? context : "unknown");
        return true;
    }
    LOG_TRACE("Drop non-IDR H264 frame while waiting for decoder recovery ({})", context ? context : "unknown");
    return false;
}

void WebRtcCtl::startWaitingForVideoKeyframe(const char *reason)
{
    if (!m_waitingForVideoKeyframe.exchange(true))
        m_videoKeyframeWaitStartedMs = QDateTime::currentMSecsSinceEpoch();
    else if (m_videoKeyframeWaitStartedMs <= 0)
        m_videoKeyframeWaitStartedMs = QDateTime::currentMSecsSinceEpoch();
    LOG_TRACE("Video receiver waiting for H264 IDR, reason={}", reason ? reason : "unknown");
}

void WebRtcCtl::requestRemoteKeyframe(const char *reason)
{
    /* 解码失败通常说明当前参考帧已损坏；同时发 RTCP PLI 和应用层请求， */
    /* 确保被控端尽快插入 IDR 恢复 VideoTrack 画面。 */
    auto now = std::chrono::steady_clock::now();
    if (m_lastKeyframeRequestTime.time_since_epoch().count() != 0 &&
        now - m_lastKeyframeRequestTime <= std::chrono::milliseconds(m_keyframeRequestBackoffMs))
    {
        return;
    }

    bool requested = false;
    if (m_videoTrack && m_videoTrack->isOpen())
    {
        requested = m_videoTrack->requestKeyframe();
    }

    if (m_inputChannel && m_inputChannel->isOpen())
    {
        QJsonObject keyframeRequest = JsonUtil::createObject()
                                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_KEYFRAME_REQUEST)
                                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                                          .add(Constant::KEY_RECEIVER, m_remoteId)
                                          .add(Constant::KEY_RECEIVER_PWD, m_remotePwdMd5)
                                          .build();
        const QJsonDocument doc(keyframeRequest);
        const QByteArray payload = doc.toJson(QJsonDocument::Compact);
        try
        {
            if (!m_inputChannel->send(std::string(payload.constData(), payload.size())))
            {
                LOG_TRACE("Keyframe request buffered by SCTP");
            }
            requested = true;
        }
        catch (const std::exception &e)
        {
            LOG_WARN("Failed to send app-level keyframe request: {}", e.what());
        }
    }

    if (requested)
    {
        m_lastKeyframeRequestTime = now;
        m_keyframeRequestBackoffMs = qMin(8000, qMax(1000, m_keyframeRequestBackoffMs * 2));
        ++m_videoFeedbackKeyframeRequests;
        maybeSendVideoAdaptFeedback(true, true, reason);
        LOG_WARN("Requested remote keyframe via RTCP/app channel, reason={}", reason ? reason : "unknown");
    }
}

void WebRtcCtl::resetVideoReceivePipeline(const char *reason, bool waitForKeyframe)
{
    {
        QMutexLocker locker(&m_videoQueueMutex);
        m_videoFrameQueue.clear();
        m_videoDrainScheduled = false;
    }
    {
        QMutexLocker locker(&m_videoReliableMutex);
        m_videoFragmentBuffer.clear();
        m_videoFragmentReceivedCount.clear();
        m_videoFragmentTimestampUs.clear();
        m_videoFragmentFirstSeenMs.clear();
        m_hasLastAcceptedReliableVideoSeq = false;
        m_lastAcceptedReliableVideoSeq = 0;
    }
    if (waitForKeyframe)
    {
        startWaitingForVideoKeyframe(reason);
        m_firstVideoPacketWithoutDecodeMs = QDateTime::currentMSecsSinceEpoch();
    }
    else
    {
        m_waitingForVideoKeyframe.store(false);
        m_videoKeyframeWaitStartedMs = 0;
    }
    if (m_h264Decoder)
        m_h264Decoder->flushBuffers();
    requestRemoteKeyframe(reason);
}

void WebRtcCtl::drainVideoFrameQueue()
{
    QueuedVideoFrame frame;
    bool hasFrame = false;
    bool shouldRequestKeyframe = false;
    {
        QMutexLocker locker(&m_videoQueueMutex);
        shouldRequestKeyframe = pruneVideoFrameQueueLocked();
        if (!m_videoFrameQueue.isEmpty())
        {
            frame = std::move(m_videoFrameQueue.dequeue());
            hasFrame = true;
        }
        else
        {
            m_videoDrainScheduled = false;
        }
    }

    if (shouldRequestKeyframe)
    {
        if (m_h264Decoder)
            m_h264Decoder->flushBuffers();
        requestRemoteKeyframe("queue-backlog");
    }

    if (!hasFrame)
        return;

    if (!shouldDecodeVideoFrame(frame.keyframe, "drain"))
    {
        bool shouldContinue = false;
        {
            QMutexLocker locker(&m_videoQueueMutex);
            shouldRequestKeyframe = pruneVideoFrameQueueLocked();
            shouldContinue = !m_videoFrameQueue.isEmpty();
            m_videoDrainScheduled = shouldContinue;
        }

        if (shouldRequestKeyframe)
        {
            if (m_h264Decoder)
                m_h264Decoder->flushBuffers();
            requestRemoteKeyframe("queue-backlog");
        }

        if (shouldContinue)
            QMetaObject::invokeMethod(this, "drainVideoFrameQueue", Qt::QueuedConnection);
        return;
    }

    processVideoFrame(frame.data, frame.info);

    bool shouldContinue = false;
    {
        QMutexLocker locker(&m_videoQueueMutex);
        shouldRequestKeyframe = pruneVideoFrameQueueLocked();
        shouldContinue = !m_videoFrameQueue.isEmpty();
        m_videoDrainScheduled = shouldContinue;
    }

    if (shouldRequestKeyframe)
    {
        if (m_h264Decoder)
            m_h264Decoder->flushBuffers();
        requestRemoteKeyframe("queue-backlog");
    }

    if (shouldContinue)
        QMetaObject::invokeMethod(this, "drainVideoFrameQueue", Qt::QueuedConnection);
}
