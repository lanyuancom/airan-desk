#include "webrtc_cli.h"

#include "audio/audio_capture_worker.h"
#include "audio/audio_playback_worker.h"
#include "../util/convert_util.h"

namespace
{
constexpr quint64 kAudioFrameDurationUs = 20000;
constexpr quint64 kAudioTimestampMaxGapUs = 2000000;

AudioCaptureWorker::CaptureSource captureSourceForAudioMode(const QString &mode)
{
    if (mode == QStringLiteral("call"))
        return AudioCaptureWorker::CaptureSource::Microphone;
    if (mode == QStringLiteral("listen"))
        return AudioCaptureWorker::CaptureSource::MicrophoneAndSystem;
    return AudioCaptureWorker::CaptureSource::MicrophoneAndSystem;
}

QString captureSourceLabel(AudioCaptureWorker::CaptureSource source)
{
    switch (source)
    {
    case AudioCaptureWorker::CaptureSource::Microphone:
        return QStringLiteral("microphone");
    case AudioCaptureWorker::CaptureSource::MicrophoneAndSystem:
        return QStringLiteral("microphone+system");
    case AudioCaptureWorker::CaptureSource::SystemLoopback:
    default:
        return QStringLiteral("system");
    }
}
} /* namespace */

quint64 WebRtcCli::normalizeAudioTimestampUs(quint64 timestamp_us)
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

void WebRtcCli::setAudioCaptureEnabled(bool enabled)
{
    if (m_audioCaptureEnabled == enabled)
        return;

    m_audioCaptureEnabled = enabled;
    if (enabled)
        startAudioCapture();
    else
        stopAudioCapture();
}

void WebRtcCli::setAudioMode(const QString &mode)
{
    QString normalized = mode.toLower();
    if (normalized != QStringLiteral("listen") && normalized != QStringLiteral("call"))
        normalized = QStringLiteral("off");

    if (m_audioMode == normalized)
        return;

    const QString previousMode = m_audioMode;
    m_audioMode = normalized;
    setAudioCaptureEnabled(m_audioMode != QStringLiteral("off"));
    if (m_audioCaptureEnabled && previousMode != QStringLiteral("off") &&
        captureSourceForAudioMode(previousMode) != captureSourceForAudioMode(m_audioMode))
    {
        stopAudioCapture();
        startAudioCapture();
    }

    if (m_audioMode == QStringLiteral("call"))
        startAudioPlayback();
    else
        stopAudioPlayback();

    LOG_INFO("Client audio mode changed: {}", m_audioMode);
}

void WebRtcCli::startAudioCapture()
{
    if (m_isOnlyFile || m_destroying || m_audioCaptureWorker || m_audioCaptureThread)
        return;

    m_audioCaptureThread = new QThread();
    m_audioCaptureThread->setObjectName(QStringLiteral("AudioCaptureWorker"));
    const AudioCaptureWorker::CaptureSource source = captureSourceForAudioMode(m_audioMode);
    m_audioCaptureWorker = new AudioCaptureWorker(source);
    m_audioCaptureWorker->moveToThread(m_audioCaptureThread);

    connect(m_audioCaptureThread, &QThread::started, m_audioCaptureWorker, &AudioCaptureWorker::start);
    connect(m_audioCaptureWorker, &AudioCaptureWorker::opusFrameReady,
            this, &WebRtcCli::onAudioFrameReady, Qt::QueuedConnection);
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
            },
            Qt::DirectConnection);

    m_audioCaptureThread->start();
    LOG_INFO("Remote audio capture enabled: source={}", captureSourceLabel(source));
}

void WebRtcCli::stopAudioCapture()
{
    if (!m_audioCaptureWorker && !m_audioCaptureThread)
        return;

    if (m_audioCaptureWorker)
        m_audioCaptureWorker->stop();

    if (m_audioCaptureThread && m_audioCaptureThread->isRunning())
    {
        m_audioCaptureThread->quit();
        if (!m_audioCaptureThread->wait(3000))
            LOG_WARN("Audio capture thread did not stop in time");
    }
    LOG_INFO("Remote audio capture disabled");
}

void WebRtcCli::handleAudioCaptureConfig(const QJsonObject &object)
{
    const bool enabled = JsonUtil::getBool(object, Constant::KEY_ENABLED, false);
    const QString mode = enabled ? JsonUtil::getString(object, Constant::KEY_AUDIO_MODE, QStringLiteral("listen")) : QStringLiteral("off");
    setAudioMode(mode);
}

void WebRtcCli::onAudioFrameReady(std::shared_ptr<rtc::binary> encodedData, quint64 timestamp_us)
{
    if (!m_connected || m_destroying || !m_audioCaptureEnabled)
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
            LOG_TRACE("Sent audio frame: {}, timestamp: {} us", ConvertUtil::formatFileSize(encodedData->size()), timestamp_us);
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to send audio frame: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Failed to send audio frame: unknown error");
    }
}

void WebRtcCli::startAudioPlayback()
{
    if (m_audioPlaybackWorker || m_audioPlaybackThread)
        return;

    m_audioPlaybackThread = new QThread();
    m_audioPlaybackThread->setObjectName(QStringLiteral("ClientAudioPlaybackWorker"));
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
    LOG_INFO("Client audio playback worker started");
}

void WebRtcCli::stopAudioPlayback()
{
    if (m_audioPlaybackWorker)
        QMetaObject::invokeMethod(m_audioPlaybackWorker, "stop", Qt::BlockingQueuedConnection);
    if (m_audioPlaybackThread && m_audioPlaybackThread->isRunning())
    {
        m_audioPlaybackThread->quit();
        if (!m_audioPlaybackThread->wait(3000))
            LOG_WARN("Client audio playback thread did not stop in time");
    }
}

void WebRtcCli::onAudioFrameReceived(rtc::binary data, rtc::FrameInfo info)
{
    if (m_audioMode != QStringLiteral("call") || data.empty() || !m_audioPlaybackWorker)
        return;

    auto payload = std::make_shared<rtc::binary>(std::move(data));
    const quint64 timestampUs = info.timestampSeconds
                                    ? static_cast<quint64>(std::chrono::duration_cast<std::chrono::microseconds>(*info.timestampSeconds).count())
                                    : static_cast<quint64>(info.timestamp);
    QMetaObject::invokeMethod(m_audioPlaybackWorker, "playOpusFrame", Qt::QueuedConnection,
                              Q_ARG(std::shared_ptr<rtc::binary>, payload),
                              Q_ARG(quint64, timestampUs));
}
