#include "webrtc_ctl.h"

#include "audio/audio_playback_worker.h"
#include "../codec/h264_decoder.h"
#include "../util/convert_util.h"

#include <atomic>
#include <QDateTime>
void WebRtcCtl::processAudioFrame(const rtc::binary &audioData, const rtc::FrameInfo &frameInfo)
{
    LOG_TRACE("Received audio frame: {}", ConvertUtil::formatFileSize(audioData.size()));

    static std::atomic<quint64> s_rxFrames{0};
    static std::atomic<quint64> s_rxBytes{0};
    static std::atomic<qint64> s_lastRxLogMs{0};
    static std::atomic<qint64> s_lastDropLogMs{0};

    if (audioData.empty())
    {
        LOG_WARN("Received empty audio frame");
        return;
    }

    s_rxFrames.fetch_add(1, std::memory_order_relaxed);
    s_rxBytes.fetch_add(static_cast<quint64>(audioData.size()), std::memory_order_relaxed);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    qint64 expectedRxLogMs = s_lastRxLogMs.load(std::memory_order_relaxed);
    if (nowMs - expectedRxLogMs >= 2000 && s_lastRxLogMs.compare_exchange_strong(expectedRxLogMs, nowMs, std::memory_order_relaxed))
    {
        const quint64 frames = s_rxFrames.exchange(0, std::memory_order_relaxed);
        const quint64 bytes = s_rxBytes.exchange(0, std::memory_order_relaxed);
        LOG_INFO("Control audio receive stats: frames={}, bytes={} in ~2s", frames, bytes);
    }

    if (m_audioMode != QStringLiteral("off") && m_audioPlaybackWorker)
    {
        auto payload = std::make_shared<rtc::binary>(audioData);
        const quint64 timestampUs = frameInfo.timestampSeconds
                                        ? static_cast<quint64>(std::chrono::duration_cast<std::chrono::microseconds>(*frameInfo.timestampSeconds).count())
                                        : static_cast<quint64>(frameInfo.timestamp);
        QMetaObject::invokeMethod(m_audioPlaybackWorker, "playOpusFrame", Qt::QueuedConnection,
                                  Q_ARG(std::shared_ptr<rtc::binary>, payload),
                                  Q_ARG(quint64, timestampUs));
    }
    else
    {
        qint64 expectedDropLogMs = s_lastDropLogMs.load(std::memory_order_relaxed);
        if (nowMs - expectedDropLogMs >= 2000 && s_lastDropLogMs.compare_exchange_strong(expectedDropLogMs, nowMs, std::memory_order_relaxed))
        {
            LOG_WARN("Control audio frame dropped before playback dispatch: mode={}, playbackWorker={}",
                     m_audioMode.toStdString(),
                     m_audioPlaybackWorker ? "ready" : "null");
        }
    }
}

/* 处理接收到的视频数据 */
void WebRtcCtl::processVideoFrame(const rtc::binary &data, const rtc::FrameInfo &frameInfo)
{
    LOG_TRACE("Received video frame: {}", ConvertUtil::formatFileSize(data.size()));

    if (data.empty())
    {
        return;
    }

    try
    {
        /* 解码H264数据为QImage */
        if (m_h264Decoder)
        {
            QImage decodedFrame = m_h264Decoder->decodeFrame(data);
            if (m_h264Decoder->lastDecodeHadError())
            {
                if (!decodedFrame.isNull())
                    LOG_WARN("Drop decoded frame because decoder reported an error; wait for next IDR");
                startWaitingForVideoKeyframe("decode-invalid");
                m_h264Decoder->flushBuffers();
                requestRemoteKeyframe("decode-invalid");
                maybeSendVideoAdaptFeedback(true, true, "decode-invalid");
                return;
            }

            if (!decodedFrame.isNull())
            {
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                m_lastVideoDecodedMs = nowMs;
                m_firstVideoPacketWithoutDecodeMs = 0;
                m_waitingForVideoKeyframe.store(false);
                m_videoKeyframeWaitStartedMs = 0;
                m_keyframeRequestBackoffMs = 1000;
                ++m_videoFeedbackDecodedFrames;
                if (m_videoStatsStartMs == 0)
                    m_videoStatsStartMs = nowMs;
                m_videoStatsBytes += static_cast<qint64>(data.size());

                emit videoFrameDecoded(decodedFrame);
                const qint64 elapsedMs = nowMs - m_videoStatsStartMs;
                if (elapsedMs >= 1000)
                {
                    const double kbps = (m_videoStatsBytes * 8.0) / elapsedMs;
                    emit videoStatsUpdated(kbps, decodedFrame.size());
                    m_videoStatsStartMs = nowMs;
                    m_videoStatsBytes = 0;
                }
                maybeSendVideoAdaptFeedback(false, false, "stable");
                LOG_TRACE("Successfully decoded video frame: {}x{}", decodedFrame.width(), decodedFrame.height());
            }
            else
            {
                LOG_TRACE("H264 decoder accepted packet but produced no display frame yet");
            }
        }
        else
        {
            LOG_WARN("H264 decoder not initialized");
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Error processing video frame: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Unknown error processing video frame");
    }
}
