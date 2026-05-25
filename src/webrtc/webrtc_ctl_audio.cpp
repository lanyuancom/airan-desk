#include "webrtc_ctl.h"

#include "audio/audio_capture_worker.h"
#include "audio/audio_playback_worker.h"
#include "opus_duplex_media_handler.h"
#include "../common/constant.h"
#include "../util/convert_util.h"
#include "../util/qt_callback_util.h"

namespace
{
constexpr quint64 kAudioFrameDurationUs = 20000;
constexpr quint64 kAudioTimestampMaxGapUs = 2000000;
} /* namespace */

quint64 WebRtcCtl::normalizeAudioTimestampUs(quint64 timestamp_us)
{
    if (!m_hasLastAudioTimestamp)
    {
        m_hasLastAudioTimestamp = true;
        m_lastAudioTimestampUs = timestamp_us > 0 ? timestamp_us : kAudioFrameDurationUs;
        return m_lastAudioTimestampUs;
    }

    const quint64 nextTimestamp = m_lastAudioTimestampUs + kAudioFrameDurationUs;
    if (timestamp_us <= m_lastAudioTimestampUs ||
        timestamp_us > m_lastAudioTimestampUs + kAudioTimestampMaxGapUs)
    {
        m_lastAudioTimestampUs = nextTimestamp;
        return m_lastAudioTimestampUs;
    }

    m_lastAudioTimestampUs = timestamp_us;
    return timestamp_us;
}

void WebRtcCtl::setAudioMode(const QString &mode)
{
    QString normalized = mode.toLower();
    if (normalized != QStringLiteral("listen") && normalized != QStringLiteral("call"))
        normalized = QStringLiteral("off");

    if (m_audioMode == normalized)
        return;

    m_audioMode = normalized;
    const bool needsRenegotiation = (m_audioMode != QStringLiteral("off") && !m_audioTrack);
    if (needsRenegotiation && !ensureAudioTrack())
        LOG_WARN("Audio mode {} selected but audio track could not be created", m_audioMode);

    if (m_audioMode == QStringLiteral("off"))
        stopAudioPlayback();
    else
        startAudioPlayback();

    if (m_audioMode == QStringLiteral("call"))
        startAudioCapture();
    else
        stopAudioCapture();

    if (needsRenegotiation && m_peerConnection)
    {
        try
        {
            m_peerConnection->setLocalDescription(rtc::Description::Type::Offer);
            LOG_INFO("Audio mode requires renegotiation offer: {}", m_audioMode);
        }
        catch (const std::exception &e)
        {
            LOG_WARN("Audio renegotiation offer failed: {}", e.what());
        }
    }

    LOG_INFO("Control audio mode changed: {}", m_audioMode);
}

bool WebRtcCtl::ensureAudioTrack()
{
    if (m_isOnlyFile || !m_peerConnection)
        return false;
    if (m_audioTrack)
        return true;

    try
    {
        std::string audioName = Constant::TYPE_AUDIO.toStdString();
        rtc::Description::Audio audioDesc(audioName);
        audioDesc.addOpusCodec(111);
        constexpr uint32_t audioSSRC = 3;
        audioDesc.addSSRC(audioSSRC, audioName, Constant::TYPE_VIDEO_MSID.toStdString(), audioName);
        audioDesc.setDirection(m_audioMode == QStringLiteral("call")
                                   ? rtc::Description::Direction::SendRecv
                                   : rtc::Description::Direction::RecvOnly);
        m_audioTrack = m_peerConnection->addTrack(audioDesc);
        if (!m_audioTrack)
            return false;
        m_audioTrack->setMediaHandler(std::make_shared<OpusDuplexMediaHandler>(audioSSRC, audioName, 111));
        m_audioTrack->onFrame(makeWeakCallback(this, &WebRtcCtl::onAudioFrameReceived));
        LOG_INFO("Control audio track created on demand, mode={}", m_audioMode);
        return true;
    }
    catch (const std::exception &e)
    {
        LOG_WARN("Failed to create control audio track: {}", e.what());
    }
    catch (...)
    {
        LOG_WARN("Failed to create control audio track: unknown error");
    }
    return false;
}

void WebRtcCtl::startAudioPlayback()
{
    if (m_audioPlaybackWorker || m_audioPlaybackThread)
        return;

    m_audioPlaybackThread = new QThread();
    m_audioPlaybackThread->setObjectName(QStringLiteral("AudioPlaybackWorker"));
    m_audioPlaybackWorker = new AudioPlaybackWorker();
    m_audioPlaybackWorker->moveToThread(m_audioPlaybackThread);

    connect(m_audioPlaybackThread, &QThread::started, m_audioPlaybackWorker, &AudioPlaybackWorker::start);
    connect(m_audioPlaybackThread, &QThread::finished, m_audioPlaybackWorker, &QObject::deleteLater);
    connect(m_audioPlaybackThread, &QThread::finished, this, [this]()
            {
                m_audioPlaybackWorker = nullptr;
                if (m_audioPlaybackThread)
                {
                    m_audioPlaybackThread->deleteLater();
                    m_audioPlaybackThread = nullptr;
                }
            });
    m_audioPlaybackThread->start();
    LOG_INFO("Audio playback worker started");
}

void WebRtcCtl::stopAudioPlayback()
{
    if (!m_audioPlaybackWorker && !m_audioPlaybackThread)
        return;
    if (m_audioPlaybackWorker)
    {
        // 立刻写原子标志：队列中所有积压的 playOpusFrame 均直接返回，
        // 不再等到 stop() 入队后才拦截，彻底消除阻塞链。
        m_audioPlaybackWorker->forceStop();
        // 异步投递 stop()，让音频设备在工作线程内正确关闭（无需主线程等待）
        QMetaObject::invokeMethod(m_audioPlaybackWorker, "stop", Qt::QueuedConnection);
    }
    if (m_audioPlaybackThread && m_audioPlaybackThread->isRunning())
    {
        m_audioPlaybackThread->quit();
        // forceStop 后积压帧已被清空，stop() 极快完成，500ms 绰绰有余
        if (!m_audioPlaybackThread->wait(500))
            LOG_WARN("Audio playback thread did not stop in time");
    }
}

void WebRtcCtl::startAudioCapture()
{
    if (m_isOnlyFile || m_audioCaptureWorker || m_audioCaptureThread)
        return;

    m_audioCaptureThread = new QThread();
    m_audioCaptureThread->setObjectName(QStringLiteral("ControlAudioCaptureWorker"));
    m_audioCaptureWorker = new AudioCaptureWorker(AudioCaptureWorker::CaptureSource::Microphone);
    m_audioCaptureWorker->moveToThread(m_audioCaptureThread);

    connect(m_audioCaptureThread, &QThread::started, m_audioCaptureWorker, &AudioCaptureWorker::start);
    connect(m_audioCaptureWorker, &AudioCaptureWorker::opusFrameReady,
            this, &WebRtcCtl::onAudioFrameReady, Qt::QueuedConnection);
    connect(m_audioCaptureWorker, &AudioCaptureWorker::stopped,
            m_audioCaptureThread, &QThread::quit, Qt::QueuedConnection);
    connect(m_audioCaptureThread, &QThread::finished,
            m_audioCaptureWorker, &QObject::deleteLater);
    connect(m_audioCaptureThread, &QThread::finished, this, [this]()
            {
                m_audioCaptureWorker = nullptr;
                if (m_audioCaptureThread)
                {
                    m_audioCaptureThread->deleteLater();
                    m_audioCaptureThread = nullptr;
                }
            });

    m_audioCaptureThread->start();
    LOG_INFO("Control microphone capture enabled");
}

void WebRtcCtl::stopAudioCapture()
{
    if (!m_audioCaptureWorker && !m_audioCaptureThread)
        return;

    if (m_audioCaptureWorker)
        m_audioCaptureWorker->stop();

    if (m_audioCaptureThread && m_audioCaptureThread->isRunning())
    {
        m_audioCaptureThread->quit();
        if (!m_audioCaptureThread->wait(3000))
            LOG_WARN("Control audio capture thread did not stop in time");
    }
    LOG_INFO("Control microphone capture disabled");
}

void WebRtcCtl::onAudioFrameReady(std::shared_ptr<rtc::binary> encodedData, quint64 timestamp_us)
{
    if (m_audioMode != QStringLiteral("call") || !m_connected)
        return;
    if (!encodedData || encodedData->empty())
        return;

    try
    {
        if (m_audioTrack && m_audioTrack->isOpen())
        {
            timestamp_us = normalizeAudioTimestampUs(timestamp_us);
            rtc::FrameInfo frameInfo{std::chrono::duration<double, std::micro>(timestamp_us)};
            m_audioTrack->sendFrame(*encodedData, frameInfo);
            LOG_TRACE("Sent control audio frame: {}, timestamp: {} us", ConvertUtil::formatFileSize(encodedData->size()), timestamp_us);
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to send control audio frame: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Failed to send control audio frame: unknown error");
    }
}
