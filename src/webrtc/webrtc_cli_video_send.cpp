#include "webrtc_cli.h"

#include "../common/constant.h"
#include "../desktop/desktop_capture_manager.h"
#include "../desktop/desktop_grab_factory.h"
#include "../util/convert_util.h"
#include "../util/json_util.h"

namespace
{
constexpr qint64 kMaxVideoPacerDebtBytes = 2 * 1024 * 1024;

uint8_t byteValue(std::byte value)
{
    return static_cast<uint8_t>(value);
}

void appendLongStartCode(rtc::binary &out)
{
    out.push_back(std::byte{0x00});
    out.push_back(std::byte{0x00});
    out.push_back(std::byte{0x00});
    out.push_back(std::byte{0x01});
}

bool hasAnnexBStartCode(const rtc::binary &data)
{
    for (size_t i = 0; i + 3 < data.size(); ++i)
    {
        if (byteValue(data[i]) != 0x00 || byteValue(data[i + 1]) != 0x00)
            continue;
        if (byteValue(data[i + 2]) == 0x01)
            return true;
        if (i + 4 < data.size() && byteValue(data[i + 2]) == 0x00 && byteValue(data[i + 3]) == 0x01)
            return true;
    }
    return false;
}

bool tryConvertLengthPrefixedH264(const rtc::binary &data, size_t lengthSize, rtc::binary &out)
{
    if (lengthSize != 2 && lengthSize != 4)
        return false;

    size_t pos = 0;
    int nalCount = 0;
    rtc::binary converted;
    converted.reserve(data.size() + 16);

    while (pos + lengthSize <= data.size())
    {
        quint32 nalSize = 0;
        for (size_t i = 0; i < lengthSize; ++i)
            nalSize = (nalSize << 8) | byteValue(data[pos + i]);

        pos += lengthSize;
        if (nalSize == 0 || nalSize > data.size() - pos)
            return false;

        appendLongStartCode(converted);
        converted.insert(converted.end(), data.begin() + pos, data.begin() + pos + nalSize);
        pos += nalSize;
        ++nalCount;
    }

    if (nalCount == 0 || pos != data.size())
        return false;

    out = std::move(converted);
    return true;
}

bool looksLikeSingleH264Nal(const rtc::binary &data)
{
    if (data.empty())
        return false;

    const uint8_t nalType = byteValue(data.front()) & 0x1F;
    return nalType > 0 && nalType < 24;
}

std::shared_ptr<rtc::binary> normalizeH264ForRtp(const std::shared_ptr<rtc::binary> &encodedData)
{
    if (!encodedData || encodedData->empty() || hasAnnexBStartCode(*encodedData))
        return encodedData;

    rtc::binary converted;
    if (tryConvertLengthPrefixedH264(*encodedData, 4, converted) ||
        tryConvertLengthPrefixedH264(*encodedData, 2, converted))
    {
        LOG_DEBUG("Converted length-prefixed H264 frame to Annex-B for RTP packetization: {} -> {} bytes",
                  encodedData->size(), converted.size());
        return std::make_shared<rtc::binary>(std::move(converted));
    }

    if (looksLikeSingleH264Nal(*encodedData))
    {
        rtc::binary withStartCode;
        withStartCode.reserve(encodedData->size() + 4);
        appendLongStartCode(withStartCode);
        withStartCode.insert(withStartCode.end(), encodedData->begin(), encodedData->end());
        LOG_DEBUG("Added Annex-B start code to raw H264 NAL for RTP packetization: {} -> {} bytes",
                  encodedData->size(), withStartCode.size());
        return std::make_shared<rtc::binary>(std::move(withStartCode));
    }

    LOG_WARN("H264 frame has no Annex-B start code and is not valid length-prefixed data; sending original {} bytes",
             encodedData->size());
    return encodedData;
}

void writeBigEndian16(rtc::binary &packet, size_t pos, quint16 value)
{
    packet[pos] = static_cast<std::byte>((value >> 8) & 0xFF);
    packet[pos + 1] = static_cast<std::byte>(value & 0xFF);
}

void writeBigEndian32(rtc::binary &packet, size_t pos, quint32 value)
{
    packet[pos] = static_cast<std::byte>((value >> 24) & 0xFF);
    packet[pos + 1] = static_cast<std::byte>((value >> 16) & 0xFF);
    packet[pos + 2] = static_cast<std::byte>((value >> 8) & 0xFF);
    packet[pos + 3] = static_cast<std::byte>(value & 0xFF);
}

void writeBigEndian64(rtc::binary &packet, size_t pos, quint64 value)
{
    for (int i = 7; i >= 0; --i)
        packet[pos + (7 - i)] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
}
} /* namespace */

void WebRtcCli::onVideoPliRequested()
{
    if (m_destroying || m_isOnlyFile)
        return;
    DesktopCaptureManager::instance()->requestKeyframe(m_subscriberId, m_screenIndex);
    LOG_INFO("Received RTCP PLI/FIR on video track, requested encoder IDR");
}

quint64 WebRtcCli::normalizeVideoTimestampUs(quint64 timestamp_us)
{
    const quint64 frameStepUs = static_cast<quint64>(1000000 / qMax(1, m_fps));
    if (!m_hasLastTimestamp)
    {
        return timestamp_us > 0 ? timestamp_us : frameStepUs;
    }

    const quint64 nextTimestamp = m_lastTimestamp + qMax<quint64>(1, frameStepUs);
    auto emitThrottled = [&](const char *kind, quint64 input, quint64 fixed) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const qint64 throttleMs = 2000;
        if (m_lastTimestampWarnMs == 0 || nowMs - m_lastTimestampWarnMs >= throttleMs)
        {
            if (m_timestampWarnSuppressed > 0)
            {
                LOG_WARN("Adjusted {} video timestamp: input={} us, fixed={} us (suppressed {} similar warnings in last {} ms)",
                         kind, input, fixed, m_timestampWarnSuppressed, throttleMs);
            }
            else
            {
                LOG_WARN("Adjusted {} video timestamp: input={} us, fixed={} us", kind, input, fixed);
            }
            m_lastTimestampWarnMs = nowMs;
            m_timestampWarnSuppressed = 0;
        }
        else
        {
            ++m_timestampWarnSuppressed;
        }
    };

    if (timestamp_us <= m_lastTimestamp)
    {
        emitThrottled("non-monotonic", timestamp_us, nextTimestamp);
        return nextTimestamp;
    }

    if (timestamp_us > nextTimestamp + frameStepUs)
    {
        emitThrottled("jumping", timestamp_us, nextTimestamp);
        return nextTimestamp;
    }

    return timestamp_us;
}

rtc::Reliability WebRtcCli::createVideoDataReliability() const
{
    rtc::Reliability reliability;
    /* Quality-first must not expire H264 fragments. If an IDR fragment is */
    /* dropped, the receiver cannot rebuild the reference chain and the screen */
    /* freezes while waiting for a keyframe that never fully arrives. */
    reliability.unordered = true;
    return reliability;
}

int WebRtcCli::effectiveBitrateKbps() const
{
    const QString profile = m_bitrateProfile.toLower();
    int medium = 3500;
    if (m_encode_width <= 854 && m_encode_height <= 480)
        medium = 1800;
    else if (m_encode_width <= 1280 && m_encode_height <= 720)
        medium = 3500;
    else if (m_encode_width <= 1920 && m_encode_height <= 1080)
        medium = 7000;
    else if (m_encode_width <= 2560 && m_encode_height <= 1440)
        medium = 12000;
    else
        medium = 18000;

    if (profile == QStringLiteral("low"))
        return qMax(500, medium * 55 / 100);
    if (profile == QStringLiteral("high"))
        return medium * 170 / 100;
    return medium;
}

int WebRtcCli::effectiveCaptureFps() const
{
    int fps = m_compatMode ? qBound(1, m_fps, 5) : qMax(1, m_fps);
    const int platformCap = DesktopGrabFactory::recommendedMaxFps();
    if (platformCap > 0 && fps > platformCap)
        fps = platformCap;
    return fps;
}

bool WebRtcCli::forceAllKeyframes() const
{
    return m_compatMode;
}

bool WebRtcCli::isH264Keyframe(const rtc::binary &data) const
{
    for (size_t i = 0; i + 4 < data.size(); ++i)
    {
        size_t start = std::string::npos;
        if (i + 3 < data.size() && byteValue(data[i]) == 0x00 && byteValue(data[i + 1]) == 0x00 && byteValue(data[i + 2]) == 0x01)
            start = i + 3;
        else if (i + 4 < data.size() && byteValue(data[i]) == 0x00 && byteValue(data[i + 1]) == 0x00 && byteValue(data[i + 2]) == 0x00 && byteValue(data[i + 3]) == 0x01)
            start = i + 4;

        if (start != std::string::npos && start < data.size())
        {
            uint8_t nalType = byteValue(data[start]) & 0x1F;
            if (nalType == 5)
                return true;
        }
    }
    return false;
}

void WebRtcCli::startMediaCapture()
{
    if (!m_isOnlyFile && !m_subscribed)
    {
        connect(DesktopCaptureManager::instance(), &DesktopCaptureManager::captureBackendChanged,
                this, &WebRtcCli::onCaptureBackendConfirmed, Qt::UniqueConnection);
        connect(DesktopCaptureManager::instance(), &DesktopCaptureManager::encoderChanged,
                this, &WebRtcCli::onCaptureEncoderConfirmed, Qt::UniqueConnection);
        /* 订阅桌面捕获管理器 */
        if (DesktopCaptureManager::instance()->subscribe(m_subscriberId, m_screenIndex, m_encode_width, m_encode_height, effectiveCaptureFps(),
                                                         effectiveBitrateKbps(), forceAllKeyframes()))
        {
            m_subscribed = true;
            DesktopCaptureManager::instance()->setCaptureBackend(m_captureBackend, m_screenIndex);
            /* 连接管理器的信号（使用 QueuedConnection 以确保跨线程安全） */
            connect(DesktopCaptureManager::instance(), &DesktopCaptureManager::frameEncoded,
                    this, &WebRtcCli::onVideoFrameReady, Qt::QueuedConnection);
            LOG_INFO("Subscribed to capture manager with resolution {}x{} and fps {}", m_encode_width, m_encode_height, effectiveCaptureFps());
        }
        else
        {
            LOG_ERROR("Failed to subscribe to capture manager");
        }
    }
}

void WebRtcCli::stopMediaCapture()
{
    LOG_INFO("Stopping media capture");
    /* 取消订阅桌面捕获管理器 */
    if (m_subscribed)
    {
        DesktopCaptureManager::instance()->unsubscribe(m_subscriberId, m_screenIndex);
        m_subscribed = false;
        LOG_INFO("Unsubscribed from capture manager");
    }

    /* 如果这是最后一个订阅者，管理器会自动停止捕获 */
    m_destroying = true;
    emit destroyCli(); /* 通知销毁客户端 */
}

void WebRtcCli::onVideoFrameReady(const QString &subscriberId, std::shared_ptr<rtc::binary> encodedData, quint64 timestamp_us)
{
    int sz = encodedData ? (int)encodedData->size() : -1;
    LOG_TRACE("onVideoFrameReady called for subscriber {} (encodedData size={})", subscriberId, sz);

    if (subscriberId != m_subscriberId || !m_connected)
        return;

    /* 验证H264数据有效性 */
    if (!encodedData || encodedData->empty())
    {
        LOG_WARN("Received empty video frame data");
        return;
    }

    encodedData = normalizeH264ForRtp(encodedData);
    if (!encodedData || encodedData->empty())
    {
        LOG_WARN("Video frame normalization produced empty data");
        return;
    }

    timestamp_us = normalizeVideoTimestampUs(timestamp_us);
    m_lastTimestamp = timestamp_us;
    m_hasLastTimestamp = true;
    const bool keyframe = isH264Keyframe(*encodedData);
    if (!m_qualityFirstMode)
    {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_videoPacerLastRefillMs <= 0)
        {
            m_videoPacerLastRefillMs = nowMs;
            m_videoPacerTokens = effectiveBitrateKbps() * 1000 / 8;
        }
        else
        {
            const qint64 elapsedMs = qBound<qint64>(0, nowMs - m_videoPacerLastRefillMs, 1000);
            const qint64 rateBytesPerSec = qMax<qint64>(64 * 1024, effectiveBitrateKbps() * 1000 / 8);
            const qint64 bucketBytes = qMax<qint64>(512 * 1024, rateBytesPerSec * 2);
            m_videoPacerTokens = qMin(bucketBytes, m_videoPacerTokens + rateBytesPerSec * elapsedMs / 1000);
            m_videoPacerLastRefillMs = nowMs;
        }

        const qint64 frameBytes = static_cast<qint64>(encodedData->size());
        if (!keyframe && m_videoPacerTokens < -kMaxVideoPacerDebtBytes)
        {
            ++m_videoPacerDroppedFrames;
            if (nowMs - m_lastVideoPacerDropLogMs > 2000)
            {
                LOG_WARN("Video pacer over budget, dropped {} P frames (debt={} bytes, bitrate={}kbps)",
                         m_videoPacerDroppedFrames, -m_videoPacerTokens, effectiveBitrateKbps());
                m_lastVideoPacerDropLogMs = nowMs;
                m_videoPacerDroppedFrames = 0;
            }
            if (nowMs - m_lastVideoPacerAdaptMs > 6000)
            {
                m_lastVideoPacerAdaptMs = nowMs;
                raiseVideoAdaptLevel(QStringLiteral("pacer-backpressure"), 1);
            }
            return;
        }
        m_videoPacerTokens -= frameBytes;
    }

    try
    {
        if (m_qualityFirstMode && trySendReliableVideoFrame(*encodedData, timestamp_us))
            return;

        if (m_qualityFirstMode)
        {
            LOG_TRACE("Reliable video data channel not open yet, skip RTP fallback for mode={}", m_streamMode);
            return;
        }

        if (m_videoTrack && m_videoTrack->isOpen())
        {
            rtc::FrameInfo frameInfo{std::chrono::duration<double, std::micro>(timestamp_us)};
            m_videoTrack->sendFrame(*encodedData, frameInfo);
            if (m_compatMode)
                DesktopCaptureManager::instance()->requestKeyframe(m_subscriberId, m_screenIndex);
            LOG_TRACE("Sent video frame: {}, timestamp: {} us", ConvertUtil::formatFileSize(encodedData->size()), timestamp_us);
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to send video frame: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Failed to send video frame: unknown error");
    }
}

bool WebRtcCli::trySendReliableVideoFrame(const rtc::binary &frame, quint64 timestamp_us)
{
    if (!m_videoDataChannel || !m_videoDataChannel->isOpen())
        return false;

    constexpr size_t kMaxVideoBufferedBytes = 4 * 1024 * 1024;
    const size_t buffered = m_videoDataChannel->bufferedAmount();
    const bool keyframe = isH264Keyframe(frame);

    if (buffered > kMaxVideoBufferedBytes)
    {
        m_videoDataCongested = true;
        if (keyframe)
        {
            m_pendingVideoKeyframe = std::make_shared<rtc::binary>(frame);
            LOG_WARN("Reliable video channel congested (buffered={} bytes), keep latest IDR and drop P frames", buffered);
        }
        return true;
    }

    if (m_videoDataCongested)
    {
        if (!keyframe)
            return true;
        m_videoDataCongested = false;
        m_pendingVideoKeyframe.reset();
        LOG_INFO("Reliable video channel recovered, resume from IDR frame");
    }

    if (!sendReliableVideoFrameFragmented(frame, timestamp_us))
        return false;

    LOG_TRACE("Sent reliable video frame: {}, timestamp: {} us", ConvertUtil::formatFileSize(frame.size()), timestamp_us);
    return true;
}

bool WebRtcCli::sendReliableVideoFrameFragmented(const rtc::binary &frame, quint64 timestamp_us)
{
    if (!m_videoDataChannel || !m_videoDataChannel->isOpen())
        return false;

    constexpr quint32 kMagic = 0x41524456; /* "ARDV" */
    constexpr quint16 kVersion = 1;
    constexpr quint16 kHeaderSize = 28;
    constexpr size_t kMaxPayload = 16 * 1024;

    const quint32 seq = ++m_videoFrameSeq;
    const quint32 totalFragments = static_cast<quint32>((frame.size() + kMaxPayload - 1) / kMaxPayload);
    if (totalFragments == 0)
        return false;

    for (quint32 index = 0; index < totalFragments; ++index)
    {
        const size_t offset = static_cast<size_t>(index) * kMaxPayload;
        const size_t payloadSize = std::min(kMaxPayload, frame.size() - offset);
        rtc::binary packet(kHeaderSize + payloadSize);

        writeBigEndian32(packet, 0, kMagic);
        writeBigEndian16(packet, 4, kVersion);
        writeBigEndian16(packet, 6, kHeaderSize);
        writeBigEndian32(packet, 8, seq);
        writeBigEndian32(packet, 12, totalFragments);
        writeBigEndian32(packet, 16, index);
        writeBigEndian64(packet, 20, timestamp_us);
        std::copy(frame.begin() + offset, frame.begin() + offset + payloadSize, packet.begin() + kHeaderSize);

        if (!m_videoDataChannel->send(packet))
            LOG_TRACE("Reliable video fragment buffered by SCTP: seq={}, {}/{}", seq, index + 1, totalFragments);
    }
    return true;
}

void WebRtcCli::flushPendingVideoKeyframe()
{
    if (!m_videoDataChannel || !m_videoDataChannel->isOpen() || !m_pendingVideoKeyframe)
        return;

    constexpr size_t kResumeBufferedBytes = 512 * 1024;
    if (m_videoDataChannel->bufferedAmount() > kResumeBufferedBytes)
        return;

    try
    {
        sendReliableVideoFrameFragmented(*m_pendingVideoKeyframe, m_lastTimestamp);
        LOG_INFO("Reliable video channel drained, resent latest pending IDR frame: {}", ConvertUtil::formatFileSize(m_pendingVideoKeyframe->size()));
        m_pendingVideoKeyframe.reset();
        m_videoDataCongested = false;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to flush pending video keyframe: {}", e.what());
    }
}

void WebRtcCli::setStreamMode(const QString &mode)
{
    QString normalized = mode.toLower();
    if (normalized != QStringLiteral("quality") &&
        normalized != QStringLiteral("smooth") &&
        normalized != QStringLiteral("compat"))
    {
        normalized = QStringLiteral("quality");
    }

    if (m_streamMode == normalized &&
        m_qualityFirstMode == (normalized == QStringLiteral("quality") || normalized == QStringLiteral("compat")) &&
        m_compatMode == (normalized == QStringLiteral("compat")))
        return;

    m_streamMode = normalized;
    m_qualityFirstMode = (m_streamMode == QStringLiteral("quality") || m_streamMode == QStringLiteral("compat"));
    m_compatMode = (m_streamMode == QStringLiteral("compat"));
    m_videoDataCongested = false;
    m_pendingVideoKeyframe.reset();
    resetVideoPacer();
    if (m_qualityFirstMode)
    {
        QMetaObject::invokeMethod(this, "recoverVideoDataChannel", Qt::QueuedConnection);
    }
    else if (m_videoDataChannelRecoverTimer)
    {
        QMetaObject::invokeMethod(m_videoDataChannelRecoverTimer, "stop", Qt::QueuedConnection);
    }
    if (m_subscribed)
    {
        if (DesktopCaptureManager::instance()->subscribe(m_subscriberId, m_screenIndex, m_encode_width, m_encode_height, effectiveCaptureFps(),
                                                         effectiveBitrateKbps(), forceAllKeyframes()))
        {
            DesktopCaptureManager::instance()->requestKeyframe(m_subscriberId, m_screenIndex);
        }
    }
    notifyCurrentStreamMode();
    LOG_INFO("Video send mode switched to {}", m_streamMode);
}

void WebRtcCli::checkControlAlive()
{
    if (m_destroying || m_isOnlyFile || !m_connected)
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_lastControlAliveMs <= 0)
        m_lastControlAliveMs = nowMs;

    constexpr qint64 kControlAliveTimeoutMs = 10000;
    if (nowMs - m_lastControlAliveMs > kControlAliveTimeoutMs)
    {
        LOG_WARN("Control side heartbeat timeout ({} ms), stop capture and destroy stale controlled session", nowMs - m_lastControlAliveMs);
        stopMediaCapture();
    }
}

void WebRtcCli::calculateEncodeResolution(int requestedMaxWidth, int requestedMaxHeight)
{
    LOG_INFO("Calculating encoding resolution - requested max: {}x{}, local screen: {}x{}",
             requestedMaxWidth, requestedMaxHeight, m_screen_width, m_screen_height);

    if (requestedMaxWidth <= 0 || requestedMaxHeight <= 0)
    {
        m_encode_width = m_screen_width;
        m_encode_height = m_screen_height;
        LOG_INFO("Using original local screen resolution: {}x{}",
                 m_encode_width, m_encode_height);
    }
    else if (m_screen_width <= requestedMaxWidth && m_screen_height <= requestedMaxHeight)
    {
        m_encode_width = m_screen_width;
        m_encode_height = m_screen_height;
        LOG_INFO("Using local screen resolution: {}x{} (fits within requested max)",
                 m_encode_width, m_encode_height);
    }
    else
    {
        double localAspectRatio = (double)m_screen_width / m_screen_height;
        double requestedAspectRatio = (double)requestedMaxWidth / requestedMaxHeight;

        if (localAspectRatio > requestedAspectRatio)
        {
            m_encode_width = requestedMaxWidth;
            m_encode_height = (int)(requestedMaxWidth / localAspectRatio);
        }
        else
        {
            m_encode_height = requestedMaxHeight;
            m_encode_width = (int)(requestedMaxHeight * localAspectRatio);
        }

        LOG_INFO("Scaled to maintain aspect ratio: {}x{} (local aspect: {:.3f}, requested aspect: {:.3f})",
                 m_encode_width, m_encode_height, localAspectRatio, requestedAspectRatio);
    }

    /* 只做偶数对齐，避免把原画缩到 16 对齐尺寸导致文字发虚。 */
    m_encode_width = m_encode_width & ~1;
    m_encode_height = m_encode_height & ~1;

    LOG_INFO("Final encoding resolution (even-aligned): {}x{}", m_encode_width, m_encode_height);
}
