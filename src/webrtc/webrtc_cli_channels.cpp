#include "webrtc_cli.h"

#include "../common/constant.h"
#include "../util/convert_util.h"
#include "../util/file_packet_util.h"
#include "../util/json_util.h"
#include "../util/qt_callback_util.h"
void WebRtcCli::setupFileChannelCallbacks()
{
    if (!m_fileChannel)
        return;

    m_fileChannel->onOpen(makeWeakCallback(this, &WebRtcCli::onFileChannelOpen));
    m_fileChannel->onMessage(makeWeakCallback(this, &WebRtcCli::onFileChannelMessage));
    m_fileChannel->onError(makeWeakCallback(this, &WebRtcCli::onFileChannelError));
    m_fileChannel->onClosed(makeWeakCallback(this, &WebRtcCli::onFileChannelClosed));
}

void WebRtcCli::setupFileTextChannelCallbacks()
{
    if (!m_fileTextChannel)
        return;

    m_fileTextChannel->onOpen(makeWeakCallback(this, &WebRtcCli::onFileTextChannelOpen));
    m_fileTextChannel->onMessage(makeWeakCallback(this, &WebRtcCli::onFileTextChannelMessage));
    m_fileTextChannel->onError(makeWeakCallback(this, &WebRtcCli::onFileTextChannelError));
    m_fileTextChannel->onClosed(makeWeakCallback(this, &WebRtcCli::onFileTextChannelClosed));
}

void WebRtcCli::onFileChannelOpen()
{
    LOG_INFO("File channel opened");
}

void WebRtcCli::onFileChannelMessage(rtc::message_variant data)
{
    if (std::holds_alternative<rtc::binary>(data))
    {
        auto binaryData = std::get<rtc::binary>(data);
        LOG_TRACE("File channel received binary data: {}", ConvertUtil::formatFileSize(binaryData.size()));
        if (m_filePacketUtil)
            m_filePacketUtil->processReceivedFragment(binaryData, "file");
    }
    else if (std::holds_alternative<std::string>(data))
    {
        LOG_WARN("File channel received text message, ignoring");
    }
}

void WebRtcCli::onFileChannelError(std::string error)
{
    LOG_ERROR("File channel error: {}", error);
}

void WebRtcCli::onFileChannelClosed()
{
    LOG_INFO("File channel closed");
    if (m_isOnlyFile)
        emit destroyCli();
}

void WebRtcCli::onFileTextChannelOpen()
{
    LOG_INFO("File text channel opened");
    populateLocalFiles();
}

void WebRtcCli::onFileTextChannelMessage(rtc::message_variant data)
{
    if (std::holds_alternative<std::string>(data))
    {
        std::string message = std::get<std::string>(data);
        QString msgStr = QString::fromUtf8(message.c_str(), static_cast<int>(message.length()));
        LOG_TRACE("File text channel received text message, size={} bytes", msgStr.toUtf8().size());

        QJsonParseError parseError{};
        QJsonDocument doc = QJsonDocument::fromJson(msgStr.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError)
        {
            LOG_ERROR("File text channel message parse error: {}", parseError.errorString());
            return;
        }

        const QJsonObject object = doc.object();
        if (JsonUtil::getString(object, Constant::KEY_MSGTYPE) == Constant::TYPE_FILE_TRANSFER_CANCEL)
        {
            const QString transferId = JsonUtil::getString(object, Constant::KEY_TRANSFER_ID);
            markTransferCancelled(transferId);
            if (m_filePacketUtil)
            {
                m_filePacketUtil->cancelTransfer(transferId);
                m_filePacketUtil->cancelAllReassemblies();
            }
            LOG_INFO("Received transfer cancel: {}", transferId);
            return;
        }

        parseFileMsg(object);
    }
    else
    {
        LOG_WARN("File text channel received binary data, ignoring");
    }
}

void WebRtcCli::onFileTextChannelError(std::string error)
{
    LOG_ERROR("File text channel error: {}", error);
}

void WebRtcCli::onFileTextChannelClosed()
{
    LOG_INFO("File text channel closed");
}

