#include "webrtc_cli.h"

#include "terminal/terminal_session.h"
#include "../common/constant.h"
#include "../util/json_util.h"

namespace
{
QString terminalDataPreview(const QByteArray &data)
{
    QString preview;
    preview.reserve(qMin(data.size(), 80) * 4);
    for (int i = 0; i < data.size() && preview.size() < 80; ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(data.at(i));
        if (ch == '\r')
            preview += QStringLiteral("\\r");
        else if (ch == '\n')
            preview += QStringLiteral("\\n");
        else if (ch == '\t')
            preview += QStringLiteral("\\t");
        else if (ch == 0x1b)
            preview += QStringLiteral("\\x1b");
        else if (ch < 0x20 || ch == 0x7f)
            preview += QStringLiteral("\\x%1").arg(ch, 2, 16, QLatin1Char('0'));
        else
            preview += QString::fromUtf8(reinterpret_cast<const char *>(&ch), 1);
    }
    if (preview.size() > 80)
        preview = preview.left(80) + QStringLiteral("...");
    else if (data.size() > preview.size())
        preview += QStringLiteral("...");
    return preview;
}
} /* namespace */

void WebRtcCli::handleTerminalMessage(const QJsonObject &object)
{
    if (QThread::currentThread() != thread())
    {
        QMetaObject::invokeMethod(this, "handleTerminalMessage",
                                  Qt::QueuedConnection,
                                  Q_ARG(QJsonObject, object));
        return;
    }

    const QString msgType = JsonUtil::getString(object, Constant::KEY_MSGTYPE);
    if (msgType == Constant::TYPE_TERMINAL_START)
    {
        if (!m_terminalSession)
        {
            m_terminalSession = new TerminalSession(this);
            connect(m_terminalSession, &TerminalSession::outputReady, this, &WebRtcCli::onTerminalOutputReady);
            connect(m_terminalSession, &TerminalSession::closed, this, &WebRtcCli::onTerminalClosed);
            connect(m_terminalSession, &TerminalSession::errorOccurred, this, &WebRtcCli::onTerminalError);
            connect(m_terminalSession, &TerminalSession::terminalInfoReady, this, &WebRtcCli::onTerminalInfoReady);
        }
        m_terminalSession->start(JsonUtil::getInt(object, Constant::KEY_COLS, 80),
                                 JsonUtil::getInt(object, Constant::KEY_ROWS, 24));
    }
    else if (msgType == Constant::TYPE_TERMINAL_INPUT)
    {
        if (!m_terminalSession)
            return;
        const QByteArray data = QByteArray::fromBase64(JsonUtil::getString(object, Constant::KEY_DATA).toLatin1());
        LOG_INFO("Terminal input received from controller: size={}, text={}", data.size(), terminalDataPreview(data));
        m_terminalSession->writeInput(data);
    }
    else if (msgType == Constant::TYPE_TERMINAL_RESIZE)
    {
        if (!m_terminalSession)
            return;
        m_terminalSession->resize(JsonUtil::getInt(object, Constant::KEY_COLS, 80),
                                  JsonUtil::getInt(object, Constant::KEY_ROWS, 24));
    }
    else if (msgType == Constant::TYPE_TERMINAL_STOP)
    {
        if (m_terminalSession)
        {
            m_terminalSession->stop();
            m_terminalSession->deleteLater();
            m_terminalSession = nullptr;
        }
    }
}

void WebRtcCli::onTerminalOutputReady(const QByteArray &data)
{
    LOG_INFO("Terminal output ready for controller: size={}, text={}", data.size(), terminalDataPreview(data));
    QJsonObject response = JsonUtil::createObject()
                               .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                               .add(Constant::KEY_MSGTYPE, Constant::TYPE_TERMINAL_OUTPUT)
                               .add(Constant::KEY_ENCODING, QStringLiteral("base64"))
                               .add(Constant::KEY_DATA, QString::fromLatin1(data.toBase64()))
                               .build();
    sendFileTextChannelMessage(response);
}

void WebRtcCli::onTerminalClosed(int exitCode)
{
    QJsonObject response = JsonUtil::createObject()
                               .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                               .add(Constant::KEY_MSGTYPE, Constant::TYPE_TERMINAL_CLOSED)
                               .add(Constant::KEY_STATUS, exitCode)
                               .build();
    sendFileTextChannelMessage(response);
}

void WebRtcCli::onTerminalError(const QString &message)
{
    QJsonObject response = JsonUtil::createObject()
                               .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                               .add(Constant::KEY_MSGTYPE, Constant::TYPE_TERMINAL_ERROR)
                               .add(Constant::KEY_ERROR, message)
                               .build();
    sendFileTextChannelMessage(response);
}

void WebRtcCli::onTerminalInfoReady(const QString &osName, const QString &shellPath, const QString &mode, bool pathTracking)
{
    QJsonObject response = JsonUtil::createObject()
                               .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                               .add(Constant::KEY_MSGTYPE, Constant::TYPE_TERMINAL_INFO)
                               .add(Constant::KEY_OS, osName)
                               .add(Constant::KEY_SHELL, shellPath)
                               .add(Constant::KEY_TERMINAL_MODE, mode)
                               .add(Constant::KEY_PATH_TRACKING, pathTracking)
                               .build();
    sendFileTextChannelMessage(response);
}
