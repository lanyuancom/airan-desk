#include "webrtc_cli.h"

#include "../common/constant.h"
#include "../util/json_util.h"
#include "../util/qt_callback_util.h"
#include "../util/screen_wake_util.h"

#include <QDateTime>
#include <QMetaObject>

void WebRtcCli::setupInputChannelCallbacks()
{
    if (!m_inputChannel)
        return;

    m_inputChannel->onOpen(makeWeakCallback(this, &WebRtcCli::onInputChannelOpen));
    m_inputChannel->onMessage(makeWeakCallback(this, &WebRtcCli::onInputChannelMessage));
    m_inputChannel->onError(makeWeakCallback(this, &WebRtcCli::onInputChannelError));
    m_inputChannel->onClosed(makeWeakCallback(this, &WebRtcCli::onInputChannelClosed));
}

void WebRtcCli::setupVideoDataChannelCallbacks()
{
    if (!m_videoDataChannel)
        return;

    m_videoDataChannel->onOpen(makeWeakCallback(this, &WebRtcCli::onVideoDataChannelOpen));
    m_videoDataChannel->onBufferedAmountLow(makeWeakCallback(this, &WebRtcCli::onVideoDataChannelBufferedAmountLow));
    m_videoDataChannel->onClosed(makeWeakCallback(this, &WebRtcCli::onVideoDataChannelClosed));
    m_videoDataChannel->onError(makeWeakCallback(this, &WebRtcCli::onVideoDataChannelError));
}

void WebRtcCli::onInputChannelOpen()
{
    if (m_inputChannelRecoverTimer)
        QMetaObject::invokeMethod(m_inputChannelRecoverTimer, "stop", Qt::QueuedConnection);
    LOG_INFO("Input channel opened");
    
    ScreenWakeUtil->wakeDisplay();
    ScreenWakeUtil->preventSleep(true);
    
    QMetaObject::invokeMethod(this, "notifyCurrentStreamMode", Qt::QueuedConnection);
    QMetaObject::invokeMethod(this, "notifyDesktopState", Qt::QueuedConnection);
}

void WebRtcCli::onInputChannelMessage(rtc::message_variant data)
{
    if (std::holds_alternative<std::string>(data))
    {
        std::string message = std::get<std::string>(data);
        QString msgStr = QString::fromUtf8(message.c_str(), static_cast<int>(message.length()));

        QJsonParseError parseError{};
        QJsonDocument doc = QJsonDocument::fromJson(msgStr.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError)
        {
            LOG_ERROR("Input channel message parse error: {}", parseError.errorString());
            return;
        }

        parseInputMsg(doc.object());
    }
}

void WebRtcCli::onInputChannelError(std::string error)
{
    LOG_ERROR("Input channel error: {}", error);
    scheduleInputChannelRecovery(QString::fromStdString(error));
}

void WebRtcCli::onInputChannelClosed()
{
    LOG_INFO("Input channel closed");
    scheduleInputChannelRecovery("closed");
}

void WebRtcCli::onVideoDataChannelOpen()
{
    if (!m_videoDataChannel)
        return;
    if (m_videoDataChannelRecoverTimer)
        QMetaObject::invokeMethod(m_videoDataChannelRecoverTimer, "stop", Qt::QueuedConnection);
    m_videoDataCongested = false;
    m_pendingVideoKeyframe.reset();
    m_videoDataChannel->setBufferedAmountLowThreshold(512 * 1024);
    LOG_INFO("Reliable video data channel opened");
    
    ScreenWakeUtil->wakeDisplay();
    ScreenWakeUtil->preventSleep(true);
}

void WebRtcCli::onVideoDataChannelBufferedAmountLow()
{
    flushPendingVideoKeyframe();
}

void WebRtcCli::onVideoDataChannelClosed()
{
    LOG_INFO("Reliable video data channel closed");
    scheduleVideoDataChannelRecovery("closed");
}

void WebRtcCli::onVideoDataChannelError(std::string error)
{
    LOG_ERROR("Reliable video data channel error: {}", error);
    scheduleVideoDataChannelRecovery(QString::fromStdString(error));
}

void WebRtcCli::scheduleInputChannelRecovery(const QString &reason)
{
    if (QThread::currentThread() != thread())
    {
        QMetaObject::invokeMethod(this, "scheduleInputChannelRecovery", Qt::QueuedConnection,
                                  Q_ARG(QString, reason));
        return;
    }

    if (m_destroying || m_isOnlyFile || !m_peerConnection)
        return;

    if (m_inputChannelRecoverTimer && m_inputChannelRecoverTimer->isActive())
        return;

    LOG_WARN("Input channel unavailable (reason: {}), schedule channel-level renegotiation", reason);
    if (m_inputChannelRecoverTimer)
        m_inputChannelRecoverTimer->start(1200);
}

void WebRtcCli::recoverInputChannel()
{
    if (m_destroying || m_isOnlyFile || !m_peerConnection)
        return;

    /* 若当前通道已经恢复，则跳过 */
    if (m_inputChannel && m_inputChannel->isOpen())
    {
        LOG_INFO("Input channel already open, skip recovery");
        return;
    }

    try
    {
        if (m_inputChannel)
        {
            try
            {
                m_inputChannel->resetCallbacks();
            }
            catch (...)
            {
            }
            try
            {
                m_inputChannel->close();
            }
            catch (...)
            {
            }
            m_inputChannel.reset();
        }

        rtc::Reliability inputReliability;
        inputReliability.unordered = true;
        inputReliability.maxRetransmits = 0;
        m_inputChannel = m_peerConnection->createDataChannel(Constant::TYPE_INPUT.toStdString(), {inputReliability});
        setupInputChannelCallbacks();

        /* 通道级恢复需要重新协商 */
        m_peerConnection->createOffer();
        LOG_INFO("Input channel recreated, renegotiation offer sent");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("recoverInputChannel failed: {}", e.what());
        if (m_inputChannelRecoverTimer)
            m_inputChannelRecoverTimer->start(2000);
    }
    catch (...)
    {
        LOG_ERROR("recoverInputChannel failed: unknown error");
        if (m_inputChannelRecoverTimer)
            m_inputChannelRecoverTimer->start(2000);
    }
}

void WebRtcCli::scheduleVideoDataChannelRecovery(const QString &reason)
{
    if (QThread::currentThread() != thread())
    {
        QMetaObject::invokeMethod(this, "scheduleVideoDataChannelRecovery", Qt::QueuedConnection,
                                  Q_ARG(QString, reason));
        return;
    }

    if (m_destroying || m_isOnlyFile || !m_peerConnection || !m_qualityFirstMode)
        return;

    if (m_videoDataChannelRecoverTimer && m_videoDataChannelRecoverTimer->isActive())
        return;

    LOG_WARN("Reliable video data channel unavailable (reason: {}), schedule channel-level renegotiation", reason);
    if (m_videoDataChannelRecoverTimer)
        m_videoDataChannelRecoverTimer->start(1200);
}

void WebRtcCli::recoverVideoDataChannel()
{
    if (m_destroying || m_isOnlyFile || !m_peerConnection)
        return;

    if (!m_qualityFirstMode)
    {
        LOG_INFO("Skip reliable video data channel recovery in smooth-first mode");
        return;
    }

    if (m_videoDataChannel && m_videoDataChannel->isOpen())
    {
        LOG_INFO("Reliable video data channel already open, skip recovery");
        return;
    }

    try
    {
        if (m_videoDataChannel)
        {
            try
            {
                m_videoDataChannel->resetCallbacks();
            }
            catch (...)
            {
            }
            try
            {
                m_videoDataChannel->close();
            }
            catch (...)
            {
            }
            m_videoDataChannel.reset();
        }

        rtc::Reliability videoReliability = createVideoDataReliability();
        m_videoDataChannel = m_peerConnection->createDataChannel("video_data", {videoReliability});
        setupVideoDataChannelCallbacks();
        m_peerConnection->createOffer();
        LOG_INFO("Reliable video data channel recreated, renegotiation offer sent");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("recoverVideoDataChannel failed: {}", e.what());
        if (m_videoDataChannelRecoverTimer)
            m_videoDataChannelRecoverTimer->start(2000);
    }
    catch (...)
    {
        LOG_ERROR("recoverVideoDataChannel failed: unknown error");
        if (m_videoDataChannelRecoverTimer)
            m_videoDataChannelRecoverTimer->start(2000);
    }
}
