#include "webrtc_cli.h"

#include "../common/constant.h"
#include "../desktop/desktop_capture_manager.h"
#include "../desktop/desktop_grab_factory.h"
#include "../util/input_util.h"
#include "../util/json_util.h"

#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <cmath>

void WebRtcCli::handleMouseEvent(const QJsonObject &object)
{
    int button = JsonUtil::getInt(object, Constant::KEY_BUTTON, -1);
    qreal x = JsonUtil::getDouble(object, Constant::KEY_X, -1);
    qreal y = JsonUtil::getDouble(object, Constant::KEY_Y, -1);
    int mouseData = JsonUtil::getInt(object, Constant::KEY_MOUSEDATA, -1);
    QString flags = JsonUtil::getString(object, Constant::KEY_DWFLAGS, "");
    if (x < 0 || y < 0)
    {
        LOG_ERROR("handleMouseEvent: Invalid mouse event data");
        return;
    }

    InputUtil::execMouseEvent(button, x, y, mouseData, flags);
    LOG_TRACE("Handled mouse event: {} at ({}, {})", flags, x, y);
}

void WebRtcCli::handleKeyboardEvent(const QJsonObject &object)
{
    int key = JsonUtil::getInt(object, Constant::KEY_KEY, -1);
    QString flags = JsonUtil::getString(object, Constant::KEY_DWFLAGS, "");
    QString text = JsonUtil::getString(object, Constant::KEY_TEXT, "");

    if (flags == QStringLiteral("text"))
    {
        if (text.isEmpty())
        {
            LOG_WARN("handleKeyboardEvent: Empty keyboard text event");
            return;
        }
        InputUtil::execKeyboardText(text);
        LOG_TRACE("Handled keyboard text event: chars={}", text.size());
        return;
    }

    if (key == -1 || flags.isEmpty())
    {
        LOG_ERROR("handleKeyboardEvent: Invalid keyboard event data");
        return;
    }

    InputUtil::execKeyboardEvent(key, flags);
    LOG_TRACE("Handled keyboard event: {} {}", flags, key);
}

void WebRtcCli::handleStreamConfig(const QJsonObject &object)
{
    const QString mode = JsonUtil::getString(object, Constant::KEY_QUALITY);
    if (mode == "quality" || mode == "smooth" || mode == "compat")
        setStreamMode(mode);
    else if (!mode.isEmpty())
        LOG_WARN("Unknown stream quality mode: {}", mode);

    if (object.contains(Constant::KEY_BITRATE_PROFILE))
    {
        const QString profile = JsonUtil::getString(object, Constant::KEY_BITRATE_PROFILE, QStringLiteral("medium")).toLower();
        if (profile == QStringLiteral("low") || profile == QStringLiteral("medium") || profile == QStringLiteral("high"))
        {
            m_bitrateProfile = profile;
            m_baseBitrateProfile = profile;
            m_adaptLevel = 0;
            m_stableVideoFeedbacks = 0;
            m_requestedEncodeWidth = m_baseRequestedEncodeWidth;
            m_requestedEncodeHeight = m_baseRequestedEncodeHeight;
            calculateEncodeResolution(m_requestedEncodeWidth, m_requestedEncodeHeight);
            if (m_baseFps > 0)
                m_fps = m_baseFps;
            if (m_subscribed)
            {
                DesktopCaptureManager::instance()->subscribe(m_subscriberId, m_screenIndex, m_encode_width, m_encode_height, effectiveCaptureFps(),
                                                             effectiveBitrateKbps(), forceAllKeyframes());
                DesktopCaptureManager::instance()->requestKeyframe(m_subscriberId, m_screenIndex);
            }
            notifyCurrentStreamMode();
            LOG_INFO("Video bitrate profile changed: {}, effective={}kbps", m_bitrateProfile, effectiveBitrateKbps());
        }
    }

    if (object.contains(Constant::KEY_CAPTURE_BACKEND))
    {
        applyCaptureBackend(JsonUtil::getString(object, Constant::KEY_CAPTURE_BACKEND));
    }

    if (object.contains(Constant::KEY_WIDTH) && object.contains(Constant::KEY_HEIGHT))
    {
        const int requestedWidth = JsonUtil::getInt(object, Constant::KEY_WIDTH, 0);
        const int requestedHeight = JsonUtil::getInt(object, Constant::KEY_HEIGHT, 0);
        applyRequestedResolution(requestedWidth, requestedHeight);
    }

    if (object.contains(Constant::KEY_FPS))
    {
        const int requestedFps = qBound(5, JsonUtil::getInt(object, Constant::KEY_FPS, m_fps), 60);
        if (requestedFps != m_fps)
        {
            m_fps = requestedFps;
            m_baseFps = requestedFps;
            m_adaptLevel = 0;
            m_stableVideoFeedbacks = 0;
            m_requestedEncodeWidth = m_baseRequestedEncodeWidth;
            m_requestedEncodeHeight = m_baseRequestedEncodeHeight;
            calculateEncodeResolution(m_requestedEncodeWidth, m_requestedEncodeHeight);
            if (m_subscribed)
            {
                DesktopCaptureManager::instance()->subscribe(m_subscriberId, m_screenIndex, m_encode_width, m_encode_height, effectiveCaptureFps(),
                                                             effectiveBitrateKbps(), forceAllKeyframes());
                DesktopCaptureManager::instance()->requestKeyframe(m_subscriberId, m_screenIndex);
            }
            notifyCurrentStreamMode();
            LOG_INFO("Video fps changed to {}", m_fps);
        }
    }
}

void WebRtcCli::handleRemoteOperation(const QJsonObject &object)
{
    const QString action = JsonUtil::getString(object, Constant::KEY_ACTION);
    QString errorMessage;
    const bool ok = InputUtil::execRemoteOperation(action, &errorMessage);
    if (ok)
    {
        LOG_INFO("Executed remote operation: {}", action);
    }
    else
    {
        LOG_WARN("Failed to execute remote operation {}: {}", action, errorMessage);
    }
}

void WebRtcCli::handleVideoAdaptFeedback(const QJsonObject &object)
{
    if (m_isOnlyFile || !m_subscribed)
        return;

    const bool congested = JsonUtil::getBool(object, QStringLiteral("congested"), false);
    const bool stalled = JsonUtil::getBool(object, QStringLiteral("stalled"), false);
    const int deltaLost = JsonUtil::getInt(object, QStringLiteral("deltaLost"), 0);
    const int deltaDropped = JsonUtil::getInt(object, QStringLiteral("deltaFramesDropped"), 0);
    const int deltaNack = JsonUtil::getInt(object, QStringLiteral("deltaNack"), 0);
    const int deltaPli = JsonUtil::getInt(object, QStringLiteral("deltaPli"), 0);
    const int decodeQueue = JsonUtil::getInt(object, QStringLiteral("decodeQueue"), 0);
    const int rttMs = JsonUtil::getInt(object, QStringLiteral("rttMs"), 0);
    const int jitterMs = JsonUtil::getInt(object, QStringLiteral("jitterMs"), 0);
    const int deltaPackets = JsonUtil::getInt(object, QStringLiteral("deltaPackets"), 0);
    const int arrivalBitrateKbps = JsonUtil::getInt(object, QStringLiteral("arrivalBitrateKbps"), 0);
    const int interArrivalJitterMs = JsonUtil::getInt(object, QStringLiteral("interArrivalJitterMs"), 0);
    const int arrivalFrames = JsonUtil::getInt(object, QStringLiteral("arrivalFrames"), 0);
    const double lossRate = (deltaLost > 0 || deltaPackets > 0)
                                ? static_cast<double>(deltaLost) / static_cast<double>(qMax(1, deltaPackets + deltaLost))
                                : 0.0;

    updateFeedbackEwma(static_cast<double>(qMax(0, arrivalBitrateKbps)), m_feedbackArrivalKbpsEwma, m_feedbackArrivalKbpsVar);
    updateFeedbackEwma(static_cast<double>(qMax(jitterMs, interArrivalJitterMs)), m_feedbackJitterMsEwma, m_feedbackJitterMsVar);
    updateFeedbackEwma(static_cast<double>(qMax(0, decodeQueue)), m_feedbackDecodeQueueEwma, m_feedbackDecodeQueueVar);
    updateFeedbackEwma(lossRate, m_feedbackLossRateEwma, m_feedbackLossRateVar);

    const double jitterStd = std::sqrt(qMax(0.0, m_feedbackJitterMsVar));
    const double queueStd = std::sqrt(qMax(0.0, m_feedbackDecodeQueueVar));
    const double lossStd = std::sqrt(qMax(0.0, m_feedbackLossRateVar));
    const int targetKbps = effectiveBitrateKbps();
    const bool arrivalStarved = arrivalFrames > 0 && arrivalBitrateKbps > 0 &&
                                arrivalBitrateKbps < qMax(256, targetKbps * 55 / 100) &&
                                arrivalBitrateKbps < m_feedbackArrivalKbpsEwma * 0.70;
    const bool dynamicQueueCongested = decodeQueue > qMax(3.0, m_feedbackDecodeQueueEwma + queueStd * 2.5);
    const bool dynamicJitterCongested = qMax(jitterMs, interArrivalJitterMs) > qMax(180.0, m_feedbackJitterMsEwma + jitterStd * 2.5);
    const bool dynamicLossCongested = lossRate > qMax(0.06, m_feedbackLossRateEwma + lossStd * 2.5);
    const bool queueCongested = dynamicQueueCongested || dynamicJitterCongested || dynamicLossCongested || arrivalStarved;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (congested || stalled || queueCongested)
    {
        m_stableVideoFeedbacks = 0;
        if (nowMs - m_lastVideoAdaptMs < 5000 && !stalled)
            return;
        if (raiseVideoAdaptLevel(QStringLiteral("feedback"), stalled ? 2 : 1))
        {
            LOG_WARN("Video feedback congested, adapt level={} stalled={} lost={} dropped={} nack={} pli={} queue={} rtt={} jitter={}",
                     m_adaptLevel, stalled, deltaLost, deltaDropped, deltaNack, deltaPli, decodeQueue, rttMs, jitterMs);
        }
        return;
    }

    if (m_adaptLevel <= 0)
        return;

    ++m_stableVideoFeedbacks;
    if (m_stableVideoFeedbacks >= 2 && nowMs - m_lastVideoAdaptMs >= 8000)
    {
        --m_adaptLevel;
        m_stableVideoFeedbacks = 0;
        m_lastVideoAdaptMs = nowMs;
        applyVideoAdaptation();
        LOG_INFO("Video feedback stable, recover adapt level={}", m_adaptLevel);
    }
}

void WebRtcCli::updateFeedbackEwma(double sample, double &mean, double &variance, double alpha)
{
    if (sample < 0.0)
        return;
    if (mean <= 0.0)
    {
        mean = sample;
        variance = 0.0;
        return;
    }

    const double diff = sample - mean;
    mean += alpha * diff;
    variance = (1.0 - alpha) * (variance + alpha * diff * diff);
}

void WebRtcCli::applyVideoAdaptation()
{
    QString targetProfile = m_baseBitrateProfile;
    int targetFps = qBound(5, m_baseFps > 0 ? m_baseFps : m_fps, 60);
    int targetRequestWidth = m_baseRequestedEncodeWidth;
    int targetRequestHeight = m_baseRequestedEncodeHeight;

    if (m_adaptLevel >= 1)
        targetProfile = QStringLiteral("low");
    if (m_adaptLevel >= 2)
        targetFps = qMax(10, targetFps * 2 / 3);
    if (m_adaptLevel >= 3)
        targetFps = qMax(8, targetFps / 2);
    if (m_adaptLevel >= 4)
        targetFps = 5;

    bool changed = false;
    if (m_bitrateProfile != targetProfile)
    {
        m_bitrateProfile = targetProfile;
        changed = true;
    }
    if (m_fps != targetFps)
    {
        m_fps = targetFps;
        changed = true;
    }
    if (m_requestedEncodeWidth != targetRequestWidth || m_requestedEncodeHeight != targetRequestHeight)
    {
        m_requestedEncodeWidth = targetRequestWidth;
        m_requestedEncodeHeight = targetRequestHeight;
        calculateEncodeResolution(m_requestedEncodeWidth, m_requestedEncodeHeight);
        changed = true;
    }

    if (!changed)
        return;

    if (DesktopCaptureManager::instance()->subscribe(m_subscriberId, m_screenIndex, m_encode_width, m_encode_height, effectiveCaptureFps(),
                                                     effectiveBitrateKbps(), forceAllKeyframes()))
    {
        resetVideoPacer();
        DesktopCaptureManager::instance()->requestKeyframe(m_subscriberId, m_screenIndex);
        notifyCurrentStreamMode();
        LOG_INFO("Applied video adaptation: level={}, profile={}, fps={}, encode={}x{}, bitrate={}kbps",
                 m_adaptLevel, m_bitrateProfile, effectiveCaptureFps(), m_encode_width, m_encode_height, effectiveBitrateKbps());
    }
    else
    {
        LOG_ERROR("Failed to apply video adaptation: level={}, profile={}, fps={}", m_adaptLevel, m_bitrateProfile, m_fps);
    }
}

bool WebRtcCli::raiseVideoAdaptLevel(const QString &reason, int step)
{
    const int nextLevel = qMin(4, m_adaptLevel + qMax(1, step));
    if (nextLevel == m_adaptLevel)
        return false;

    m_adaptLevel = nextLevel;
    m_stableVideoFeedbacks = 0;
    m_lastVideoAdaptMs = QDateTime::currentMSecsSinceEpoch();
    applyVideoAdaptation();
    LOG_WARN("Raised video adaptation level to {} by {}", m_adaptLevel, reason);
    return true;
}

void WebRtcCli::resetVideoPacer()
{
    m_videoPacerLastRefillMs = 0;
    m_videoPacerTokens = 0;
    m_videoPacerDroppedFrames = 0;
    m_lastVideoPacerDropLogMs = 0;
    m_lastTimestamp = 0;
    m_hasLastTimestamp = false;
    m_lastTimestampWarnMs = 0;
    m_timestampWarnSuppressed = 0;
}

void WebRtcCli::handleRunFile(const QJsonObject &object)
{
    const QString filePath = JsonUtil::getString(object, Constant::KEY_PATH_CLI,
                                                JsonUtil::getString(object, Constant::KEY_PATH));
    if (filePath.isEmpty())
    {
        LOG_WARN("handleRunFile ignored empty path");
        return;
    }

    QString errorMessage;
    const bool ok = InputUtil::runProgram(filePath, &errorMessage);
    QJsonObject response = JsonUtil::createObject()
                               .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                               .add(Constant::KEY_MSGTYPE, Constant::TYPE_RUN_FILE)
                               .add(Constant::KEY_PATH_CLI, filePath)
                               .add(Constant::KEY_STATUS, ok)
                               .add(Constant::KEY_ERROR, errorMessage)
                               .build();
    sendFileTextChannelMessage(response);
}

void WebRtcCli::applyCaptureBackend(const QString &backend)
{
    const QString normalized = DesktopGrabFactory::normalizeBackend(backend);
    if (normalized == m_captureBackend)
    {
        LOG_INFO("Capture backend request unchanged: {}", normalized);
        if (!m_subscribed)
        {
            notifyCurrentStreamMode();
        }
        return;
    }

    if (!DesktopCaptureManager::instance()->availableCaptureBackends().contains(normalized))
    {
        LOG_WARN("Requested capture backend {} is not available", normalized);
        notifyCurrentStreamMode();
        return;
    }

    m_captureBackend = normalized;
    LOG_INFO("Applying capture backend request: {}", m_captureBackend);
    if (m_subscribed)
    {
        DesktopCaptureManager::instance()->setCaptureBackend(m_captureBackend, m_screenIndex);
    }
}

void WebRtcCli::onCaptureBackendConfirmed(int screenIndex, const QString &requestedBackend, const QString &actualBackend)
{
    if (screenIndex != m_screenIndex || actualBackend.isEmpty())
        return;

    m_captureBackend = actualBackend;
    LOG_INFO("Capture backend confirmed by worker: requested={}, actual={}", requestedBackend, actualBackend);
    notifyCurrentStreamMode();
}

void WebRtcCli::onCaptureEncoderConfirmed(int screenIndex, const QString &encoderName, bool isHardware, bool zeroCopy)
{
    if (screenIndex != m_screenIndex || encoderName.isEmpty())
        return;

    m_encoderName = encoderName;
    m_encoderIsHardware = isHardware;
    m_encoderZeroCopy = zeroCopy;
    LOG_INFO("Capture encoder confirmed by worker: encoder={}, hardware={}, zeroCopy={}", encoderName, isHardware, zeroCopy);
    notifyCurrentStreamMode();
}

void WebRtcCli::handleSwitchScreen(const QJsonObject &object)
{
    Q_UNUSED(object);
    m_adaptLevel = 0;
    m_stableVideoFeedbacks = 0;
    m_requestedEncodeWidth = m_baseRequestedEncodeWidth;
    m_requestedEncodeHeight = m_baseRequestedEncodeHeight;
    switchToNextScreen();
}

void WebRtcCli::applyRequestedResolution(int requestedWidth, int requestedHeight)
{
    if (requestedWidth < 0 || requestedHeight < 0)
    {
        m_requestedEncodeWidth = -1;
        m_requestedEncodeHeight = -1;
        m_baseRequestedEncodeWidth = requestedWidth;
        m_baseRequestedEncodeHeight = requestedHeight;
        m_adaptLevel = 0;
        m_stableVideoFeedbacks = 0;
        calculateEncodeResolution(-1, -1);
        LOG_INFO("Transfer resolution changed to original screen resolution: {}x{}",
                 m_encode_width, m_encode_height);
    }
    else
    {
        m_requestedEncodeWidth = requestedWidth;
        m_requestedEncodeHeight = requestedHeight;
        m_baseRequestedEncodeWidth = requestedWidth;
        m_baseRequestedEncodeHeight = requestedHeight;
        m_adaptLevel = 0;
        m_stableVideoFeedbacks = 0;
        calculateEncodeResolution(m_requestedEncodeWidth, m_requestedEncodeHeight);
        LOG_INFO("Transfer resolution requested by control side: {}x{}, actual encode {}x{}",
                 requestedWidth, requestedHeight, m_encode_width, m_encode_height);
    }

    if (!m_subscribed)
    {
        LOG_INFO("Stored transfer resolution, media capture is not subscribed yet");
        return;
    }

    if (DesktopCaptureManager::instance()->subscribe(m_subscriberId, m_screenIndex, m_encode_width, m_encode_height, effectiveCaptureFps(),
                                                     effectiveBitrateKbps(), forceAllKeyframes()))
    {
        resetVideoPacer();
        LOG_INFO("Updated capture subscription to {}x{} @ {}fps", m_encode_width, m_encode_height, effectiveCaptureFps());
        DesktopCaptureManager::instance()->requestKeyframe(m_subscriberId, m_screenIndex);
        m_videoDataCongested = false;
        m_pendingVideoKeyframe.reset();
        notifyCurrentStreamMode();
    }
    else
    {
        LOG_ERROR("Failed to update capture subscription to {}x{}", m_encode_width, m_encode_height);
    }
}

void WebRtcCli::switchToNextScreen()
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    const int screenCount = screens.size();
    if (screenCount <= 1)
    {
        LOG_INFO("Switch screen ignored: only {} screen available", screenCount);
        return;
    }

    const int oldScreenIndex = m_screenIndex;
    const int newScreenIndex = (m_screenIndex + 1) % screenCount;
    QScreen *screen = screens.value(newScreenIndex, nullptr);
    QRect screenGeometry = screen ? screen->geometry() : QRect(0, 0, 1920, 1080);

    m_screenIndex = newScreenIndex;
    m_screen_width = screenGeometry.width();
    m_screen_height = screenGeometry.height();
    if (m_requestedEncodeWidth < 0 || m_requestedEncodeHeight < 0)
    {
        calculateEncodeResolution(-1, -1);
    }
    else if (m_requestedEncodeWidth > 0 && m_requestedEncodeHeight > 0)
    {
        calculateEncodeResolution(m_requestedEncodeWidth, m_requestedEncodeHeight);
    }
    else
    {
        calculateEncodeResolution(-1, -1);
    }

    if (m_subscribed)
    {
        DesktopCaptureManager::instance()->unsubscribe(m_subscriberId, oldScreenIndex);
        m_subscribed = false;
        if (DesktopCaptureManager::instance()->subscribe(m_subscriberId, m_screenIndex, m_encode_width, m_encode_height, effectiveCaptureFps(),
                                                         effectiveBitrateKbps(), forceAllKeyframes()))
        {
            m_subscribed = true;
            DesktopCaptureManager::instance()->setCaptureBackend(m_captureBackend, m_screenIndex);
            LOG_INFO("Switched capture screen from {} to {} ({}x{}, encode {}x{} @ {}fps)",
                     oldScreenIndex, m_screenIndex, m_screen_width, m_screen_height, m_encode_width, m_encode_height, effectiveCaptureFps());
        }
        else
        {
            LOG_ERROR("Failed to subscribe capture manager after switching to screen {}", m_screenIndex);
        }
    }
    else
    {
        LOG_INFO("Prepared capture screen switch from {} to {}, media capture is not subscribed yet", oldScreenIndex, m_screenIndex);
    }
}
