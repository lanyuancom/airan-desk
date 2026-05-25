#include "webrtc_ctl.h"

#include "../common/constant.h"
#include "../util/convert_util.h"
#include "../util/file_packet_util.h"
#include "../util/json_util.h"
#include "../util/qt_callback_util.h"

#include <QDateTime>
#include <QMetaObject>
void WebRtcCtl::setupFileChannelCallbacks()
{
    if (!m_fileChannel)
        return;

    m_fileChannel->onOpen(makeWeakCallback(this, &WebRtcCtl::onFileChannelOpen));
    m_fileChannel->onClosed(makeWeakCallback(this, &WebRtcCtl::onFileChannelClosed));
    m_fileChannel->onError(makeWeakCallback(this, &WebRtcCtl::onFileChannelError));
    m_fileChannel->onMessage(makeWeakCallback(this, &WebRtcCtl::onFileChannelMessage));
}

void WebRtcCtl::setupFileTextChannelCallbacks()
{
    if (!m_fileTextChannel)
        return;

    m_fileTextChannel->onOpen(makeWeakCallback(this, &WebRtcCtl::onFileTextChannelOpen));
    m_fileTextChannel->onClosed(makeWeakCallback(this, &WebRtcCtl::onFileTextChannelClosed));
    m_fileTextChannel->onError(makeWeakCallback(this, &WebRtcCtl::onFileTextChannelError));
    m_fileTextChannel->onMessage(makeWeakCallback(this, &WebRtcCtl::onFileTextChannelMessage));
}

void WebRtcCtl::setupInputChannelCallbacks()
{
    if (!m_inputChannel)
        return;

    m_inputChannel->onOpen(makeWeakCallback(this, &WebRtcCtl::onInputChannelOpen));
    m_inputChannel->onClosed(makeWeakCallback(this, &WebRtcCtl::onInputChannelClosed));
    m_inputChannel->onError(makeWeakCallback(this, &WebRtcCtl::onInputChannelError));
    m_inputChannel->onMessage(makeWeakCallback(this, &WebRtcCtl::onInputChannelMessage));
}

void WebRtcCtl::onFileChannelOpen()
{
    const QString channelLabel = m_fileChannel ? QString::fromStdString(m_fileChannel->label()) : QString();
    LOG_INFO("File channel opened: {}", channelLabel);
}

void WebRtcCtl::onFileChannelClosed()
{
    const QString channelLabel = m_fileChannel ? QString::fromStdString(m_fileChannel->label()) : QString();
    LOG_INFO("File channel closed: {}", channelLabel);
}

void WebRtcCtl::onFileChannelError(const std::string &error)
{
    LOG_ERROR("File channel error: {}", error);
}

void WebRtcCtl::onFileChannelMessage(const rtc::message_variant &message)
{
    const QString channelLabel = m_fileChannel ? QString::fromStdString(m_fileChannel->label()) : QString();
    if (std::holds_alternative<rtc::binary>(message))
    {
        auto binaryData = std::get<rtc::binary>(message);
        LOG_DEBUG("File channel received binary data: {}", ConvertUtil::formatFileSize(binaryData.size()));

        if (m_filePacketUtil)
            m_filePacketUtil->processReceivedFragment(binaryData, channelLabel);
    }
    else if (std::holds_alternative<std::string>(message))
    {
        LOG_WARN("File channel received text message, but should use file_text channel instead");
    }
}

void WebRtcCtl::onFileTextChannelOpen()
{
    const QString channelLabel = m_fileTextChannel ? QString::fromStdString(m_fileTextChannel->label()) : QString();
    LOG_INFO("File text channel opened: {}", channelLabel);
    emit fileTextChannelOpened();
}

void WebRtcCtl::onFileTextChannelClosed()
{
    const QString channelLabel = m_fileTextChannel ? QString::fromStdString(m_fileTextChannel->label()) : QString();
    LOG_INFO("File text channel closed: {}", channelLabel);
}

void WebRtcCtl::onFileTextChannelError(const std::string &error)
{
    LOG_ERROR("File text channel error: {}", error);
}

void WebRtcCtl::onFileTextChannelMessage(const rtc::message_variant &message)
{
    if (std::holds_alternative<std::string>(message))
    {
        std::string data = std::get<std::string>(message);
        QByteArray dataArr = QByteArray::fromStdString(data);
        LOG_TRACE("File text channel received text message, size={} bytes", dataArr.size());

        QJsonObject object = JsonUtil::safeParseObject(dataArr);
        if (JsonUtil::isValidObject(object))
        {
            const QString msgType = JsonUtil::getString(object, Constant::KEY_MSGTYPE);
            LOG_TRACE("Parsed file text channel message type={}, size={} bytes", msgType, dataArr.size());

            if (msgType == Constant::TYPE_TERMINAL_OUTPUT)
            {
                const QByteArray bytes = QByteArray::fromBase64(JsonUtil::getString(object, Constant::KEY_DATA).toLatin1());
                emit terminalOutput(bytes);
            }
            else if (msgType == Constant::TYPE_TERMINAL_INFO)
            {
                emit terminalInfo(JsonUtil::getString(object, Constant::KEY_OS),
                                  JsonUtil::getString(object, Constant::KEY_SHELL),
                                  JsonUtil::getString(object, Constant::KEY_TERMINAL_MODE),
                                  JsonUtil::getBool(object, Constant::KEY_PATH_TRACKING, true));
            }
            else if (msgType == Constant::TYPE_TERMINAL_CLOSED)
            {
                emit terminalClosed(JsonUtil::getInt(object, Constant::KEY_STATUS, 0));
            }
            else if (msgType == Constant::TYPE_TERMINAL_ERROR)
            {
                emit terminalError(JsonUtil::getString(object, Constant::KEY_ERROR));
            }
            else if (msgType == Constant::TYPE_UPLOAD_FILE_RES)
            {
                const QString cliPath = JsonUtil::getString(object, Constant::KEY_PATH_CLI);
                const bool status = JsonUtil::getBool(object, "status");
                LOG_INFO("Upload response: {} - {}", cliPath, status);
                emit recvUploadFileRes(status, cliPath);
            }
            else if (msgType == Constant::TYPE_FILE_LIST)
            {
                emit recvGetFileList(object);
            }
            else if (msgType == Constant::TYPE_FILE_TRANSFER_PROGRESS)
            {
                emitTransferProgress(JsonUtil::getString(object, Constant::KEY_TRANSFER_ID),
                                     JsonUtil::getInt64(object, Constant::KEY_TRANSFER_BYTES),
                                     JsonUtil::getInt64(object, Constant::KEY_TRANSFER_TOTAL_BYTES),
                                     JsonUtil::getInt(object, Constant::KEY_TRANSFER_FILE_COUNT, 0),
                                     JsonUtil::getInt(object, Constant::KEY_TRANSFER_TOTAL_FILES, 0));
            }
            else if (msgType == Constant::TYPE_FILE_TRANSFER_CANCEL)
            {
                const QString transferId = JsonUtil::getString(object, Constant::KEY_TRANSFER_ID);
                markTransferCancelled(transferId);
                if (m_filePacketUtil)
                {
                    m_filePacketUtil->cancelTransfer(transferId);
                    m_filePacketUtil->cancelAllReassemblies();
                }
            }
            else if (msgType == Constant::TYPE_FILE_DOWNLOAD)
            {
                if (object.contains(Constant::KEY_ERROR))
                {
                    QString ctlPath = JsonUtil::getString(object, Constant::KEY_PATH_CTL,
                                                          JsonUtil::getString(object, Constant::KEY_PATH));
                    LOG_WARN("Download failed remotely: path={}, error={}",
                             ctlPath,
                             JsonUtil::getString(object, Constant::KEY_ERROR));
                    emit recvDownloadFile(false, ctlPath);
                }
                else if (object.contains("directoryEnd"))
                {
                    QString ctlPath = JsonUtil::getString(object, Constant::KEY_PATH_CTL);
                    bool status = JsonUtil::getBool(object, "status", true);
                    emit recvDownloadFile(status, ctlPath);
                }
            }
            else if (msgType == Constant::TYPE_RUN_FILE)
            {
                LOG_INFO("Run file response: path={}, status={}, error={}",
                         JsonUtil::getString(object, Constant::KEY_PATH_CLI),
                         JsonUtil::getBool(object, Constant::KEY_STATUS, false),
                         JsonUtil::getString(object, Constant::KEY_ERROR));
            }
            else
            {
                emit recvGetFileList(object);
            }
        }
        else
        {
            LOG_ERROR("Failed to parse file text channel JSON message, size={} bytes", dataArr.size());
        }
    }
    else
    {
        LOG_WARN("File text channel received binary data, ignoring");
    }
}

void WebRtcCtl::onInputChannelOpen()
{
    const QString channelLabel = m_inputChannel ? QString::fromStdString(m_inputChannel->label()) : QString();
    LOG_INFO("Input channel opened: {}", channelLabel);
    sendStreamConfig();
    if (!m_isOnlyFile && m_controlHeartbeatTimer)
    {
        QMetaObject::invokeMethod(m_controlHeartbeatTimer, "start",
                                  Qt::QueuedConnection, Q_ARG(int, 2000));
        QMetaObject::invokeMethod(this, "sendControlHeartbeat", Qt::QueuedConnection);
    }
}

void WebRtcCtl::sendStreamConfig()
{
    if (m_isOnlyFile || !m_inputChannel || !m_inputChannel->isOpen())
        return;

    int requestedFps = ConfigUtil->fps;
#if QT_POINTER_SIZE == 4
    if (requestedFps > 10)
        requestedFps = 10;
#endif

    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_STREAM_CONFIG)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, m_remoteId)
                          .add(Constant::KEY_RECEIVER_PWD, m_remotePwdMd5)
                          .add(Constant::KEY_QUALITY, m_streamMode)
                          .add(Constant::KEY_BITRATE_PROFILE, m_bitrateProfile)
                          .add(Constant::KEY_NETWORK_PATH, m_networkPath)
                          .add(Constant::KEY_CAPTURE_BACKEND, m_captureBackend)
                          .add(Constant::KEY_WIDTH, m_requestedWidth)
                          .add(Constant::KEY_HEIGHT, m_requestedHeight)
                          .add(Constant::KEY_FPS, requestedFps)
                          .build();

    try
    {
        m_inputChannel->send(rtc::message_variant(JsonUtil::toCompactBytes(obj).toStdString()));
        LOG_INFO("Initial stream config sent: quality={}, bitrateProfile={}, networkPath={}, resolution=original, fps={}",
                 m_streamMode, m_bitrateProfile, m_networkPath, requestedFps);
    }
    catch (const std::exception &e)
    {
        LOG_WARN("Failed to send initial stream config: {}", e.what());
    }
}

void WebRtcCtl::onInputChannelClosed()
{
    const QString channelLabel = m_inputChannel ? QString::fromStdString(m_inputChannel->label()) : QString();
    LOG_INFO("Input channel closed: {}", channelLabel);
    if (m_controlHeartbeatTimer)
        QMetaObject::invokeMethod(m_controlHeartbeatTimer, "stop", Qt::QueuedConnection);
    scheduleReconnect();
}

void WebRtcCtl::onInputChannelError(const std::string &error)
{
    LOG_ERROR("Input channel error: {}", error);
    if (m_controlHeartbeatTimer)
        QMetaObject::invokeMethod(m_controlHeartbeatTimer, "stop", Qt::QueuedConnection);
    scheduleReconnect();
}

void WebRtcCtl::onInputChannelMessage(const rtc::message_variant &message)
{
    if (std::holds_alternative<std::string>(message))
    {
        const std::string data = std::get<std::string>(message);
        QJsonObject object = JsonUtil::safeParseObject(QByteArray::fromStdString(data));
        const QString msgType = JsonUtil::getString(object, Constant::KEY_MSGTYPE);
        if (msgType == Constant::TYPE_STREAM_CONFIG)
        {
            applyLocalStreamConfig(object);
        }
        else if (msgType == Constant::TYPE_DESKTOP_STATE)
        {
            const bool locked = JsonUtil::getBool(object, Constant::KEY_LOCKED, false);
            const QString messageText = JsonUtil::getString(object, Constant::KEY_MESSAGE);
            Q_EMIT desktopStateChanged(locked, messageText);
            LOG_INFO("Desktop state updated: locked={}, message={}", locked, messageText);
        }
        else
        {
            LOG_DEBUG("Input channel message received (control side)");
        }
    }
    else
    {
        LOG_DEBUG("Input channel binary message received (control side)");
    }
}
