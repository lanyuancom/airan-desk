#include "webrtc_cli.h"

#include "../common/constant.h"
#include "../desktop/desktop_capture_manager.h"
#include "../util/json_util.h"

namespace
{
QString localOsName()
{
#if defined(Q_OS_ANDROID)
    return QStringLiteral("android");
#elif defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return QStringLiteral("unknown");
#endif
}
} /* namespace */

void WebRtcCli::sendFileChannelMessage(const QJsonObject &message)
{
    if (!m_connected || !m_fileChannel || !m_fileChannel->isOpen())
    {
        LOG_ERROR("File channel not available");
        return;
    }

    QString jsonStr = JsonUtil::toCompactString(message);
    std::string stdStr = jsonStr.toStdString();

    try
    {
        m_fileChannel->send(stdStr);
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

void WebRtcCli::sendFileTextChannelMessage(const QJsonObject &message)
{
    if (!m_connected || !m_fileTextChannel || !m_fileTextChannel->isOpen())
    {
        LOG_ERROR("File text channel not available");
        return;
    }

    QString jsonStr = JsonUtil::toCompactString(message);
    std::string stdStr = jsonStr.toStdString();

    try
    {
        const bool sent = m_fileTextChannel->send(stdStr);
        const QString msgType = JsonUtil::getString(message, Constant::KEY_MSGTYPE);
        if (!sent)
        {
            LOG_WARN("File text channel send returned false: type={}, size={} bytes, buffered={} bytes",
                     msgType,
                     jsonStr.toUtf8().size(),
                     m_fileTextChannel->bufferedAmount());
        }
        else if (msgType == Constant::TYPE_TERMINAL_OUTPUT)
        {
            LOG_INFO("Terminal output message sent to controller: size={} bytes, buffered={} bytes",
                     jsonStr.toUtf8().size(),
                     m_fileTextChannel->bufferedAmount());
        }
        else
        {
            LOG_TRACE("Sent file text channel message type={}, size={} bytes",
                      msgType,
                      jsonStr.toUtf8().size());
        }
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

void WebRtcCli::sendInputChannelMessage(const QJsonObject &message)
{
    if (!m_inputChannel || !m_inputChannel->isOpen())
    {
        LOG_ERROR("Input channel not available");
        return;
    }

    QString jsonStr = JsonUtil::toCompactString(message);
    std::string stdStr = jsonStr.toStdString();

    try
    {
        m_inputChannel->send(stdStr);
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

void WebRtcCli::setDesktopLocked(bool locked)
{
    if (m_desktopLocked == locked)
        return;

    m_desktopLocked = locked;
    notifyDesktopState();
}

void WebRtcCli::notifyDesktopState()
{
    if (m_isOnlyFile)
        return;
    if (!m_inputChannel || !m_inputChannel->isOpen())
        return;

    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_DESKTOP_STATE)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, m_remoteId)
                          .add(Constant::KEY_LOCKED, m_desktopLocked)
                          .add(Constant::KEY_MESSAGE, m_desktopLocked
                                                       ? QStringLiteral("Remote Windows desktop is locked. Current app-mode capture cannot show the password screen.")
                                                       : QStringLiteral("Remote Windows desktop is unlocked."))
                          .build();
    sendInputChannelMessage(obj);
    LOG_INFO("Notified control side desktop lock state: {}", m_desktopLocked);
}

void WebRtcCli::notifyCurrentStreamMode()
{
    if (m_isOnlyFile)
        return;

    QStringList backends = DesktopCaptureManager::instance()->availableCaptureBackends();
    backends.removeAll(QStringLiteral("auto"));

    const QString signature = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11")
                                  .arg(m_streamMode)
                                  .arg(m_bitrateProfile)
                                  .arg(effectiveBitrateKbps())
                                  .arg(m_networkPath)
                                  .arg(m_captureBackend)
                                  .arg(m_encoderName)
                                  .arg(m_encoderIsHardware ? QStringLiteral("hw") : QStringLiteral("sw"))
                                  .arg(m_encoderZeroCopy ? QStringLiteral("zc1") : QStringLiteral("zc0"))
                                  .arg(m_adaptLevel)
                                  .arg(backends.join(QLatin1Char(',')))
                                  .arg(m_adaptLevel > 0 ? QStringLiteral("a1") : QStringLiteral("a0"));
    if (signature == m_lastStreamConfigSignature)
        return;
    m_lastStreamConfigSignature = signature;

    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_STREAM_CONFIG)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, m_remoteId)
                          .add("statusOnly", true)
                          .add("adaptive", m_adaptLevel > 0)
                          .add("adaptLevel", m_adaptLevel)
                          .add(Constant::KEY_QUALITY, m_streamMode)
                          .add(Constant::KEY_BITRATE_PROFILE, m_bitrateProfile)
                          .add(Constant::KEY_BITRATE, effectiveBitrateKbps())
                          .add(Constant::KEY_NETWORK_PATH, m_networkPath)
                          .add(Constant::KEY_OS, localOsName())
                          .add(Constant::KEY_CAPTURE_BACKEND, m_captureBackend)
                          .add(Constant::KEY_CAPTURE_BACKENDS, QJsonArray::fromStringList(backends))
                          .add("encoderName", m_encoderName)
                          .add("encoderType", m_encoderIsHardware ? "hardware" : "software")
                          .add("encoderZeroCopy", m_encoderZeroCopy)
                          .build();
    sendInputChannelMessage(obj);
    LOG_INFO("Notified control side stream config: capture backend={}", m_captureBackend);
}
