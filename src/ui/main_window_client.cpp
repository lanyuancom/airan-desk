#include "main_window.h"
#include "control_window.h"
#include "file_transfer_window.h"
#include "terminal_window.h"
#include "common/constant.h"
#include "util/json_util.h"
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRadioButton>
#include <QThread>
#include <QUuid>
#include <QUrl>
#include <QUrlQuery>

void MainWindow::cleanupWebRtcCliSessions()
{
    if (m_rtcCliSessions.isEmpty())
    {
        return;
    }

    auto sessions = m_rtcCliSessions;
    m_rtcCliSessions.clear();

    for (auto it = sessions.begin(); it != sessions.end(); ++it)
    {
        WebRtcCli *webrtcCli = it.key();
        QThread *rtcCliThread = it.value();

        if (!webrtcCli)
        {
            if (rtcCliThread)
            {
                STOP_PTR_THREAD(rtcCliThread);
                delete rtcCliThread;
            }
            continue;
        }

        if (rtcCliThread && rtcCliThread->isRunning())
        {
            QMetaObject::invokeMethod(webrtcCli, "destroy", Qt::BlockingQueuedConnection);
            webrtcCli->disconnect();
            QMetaObject::invokeMethod(webrtcCli, "deleteLater", Qt::BlockingQueuedConnection);
            STOP_PTR_THREAD(rtcCliThread);
        }
        else
        {
            DELETE_PTR_FUNC(webrtcCli);
        }

        if (rtcCliThread)
        {
            delete rtcCliThread;
        }
    }

    LOG_INFO("All WebRtcCli sessions cleaned up");
}

void MainWindow::initCli()
{
    /* 连接websocket相关信号 */
    connect(&m_ws, &WsCli::onWsCliConnected, this, &MainWindow::onWsCliConnected);
    connect(&m_ws, &WsCli::onWsCliDisconnected, this, &MainWindow::onWsCliDisconnected);
    connect(&m_ws, &WsCli::onReconnectStatusUpdate, this, &MainWindow::onWsCliReconnectStatus);
    connect(&m_ws, &WsCli::onWsCliRecvBinaryMsg, this, &MainWindow::onWsCliRecvBinaryMsg);
    connect(&m_ws, &WsCli::onWsCliRecvTextMsg, this, &MainWindow::onWsCliRecvTextMsg);
    connect(this, &MainWindow::initWsCli, &m_ws, &WsCli::init);
    connect(this, &MainWindow::resetWsCliUrl, &m_ws, &WsCli::resetUrlAndReconnect);
    connect(this, &MainWindow::sendWsCliBinaryMsg, &m_ws, &WsCli::sendWsCliBinaryMsg);
    connect(this, &MainWindow::sendWsCliTextMsg, &m_ws, &WsCli::sendWsCliTextMsg);

    /*
    将WebSocket客户端移动到工作线程，避免阻塞UI线程。WsCli内部会处理线程安全问题，并且设计为在其自己的线程中自动重连和发送心跳。
    */
    m_ws_thread.setObjectName("WsCliThread");
    m_ws.moveToThread(&m_ws_thread);
    m_ws_thread.start();

    emit initWsCli(buildWsUrl(), 30 * 1000);
}

QString MainWindow::buildWsUrl() const
{
    QUrl url(ConfigUtil->wsUrl);
    QUrlQuery query(url);
    query.removeQueryItem("sessionId");
    query.removeQueryItem("hostname");
    query.removeQueryItem("installId");
    query.addQueryItem("sessionId", ConfigUtil->local_id);
    query.addQueryItem("hostname", QHostInfo::localHostName());
    query.addQueryItem("installId", ConfigUtil->install_id);
    url.setQuery(query);
    return url.toString();
}

void MainWindow::handleDeviceIdConflict(const QJsonObject &object)
{
    QJsonValue dataVal = object.value(Constant::KEY_DATA);
    if (!dataVal.isObject())
    {
        LOG_ERROR("Invalid data object in DEVICE_ID_CONFLICT message");
        return;
    }

    const QJsonObject data = dataVal.toObject();
    const QString newSessionId = JsonUtil::getString(data, Constant::KEY_NEW_SESSION_ID);
    if (newSessionId.isEmpty() || QUuid(newSessionId).isNull())
    {
        LOG_ERROR("Missing or invalid newSessionId in DEVICE_ID_CONFLICT message");
        return;
    }

    LOG_WARN("Device id conflict detected, replacing local id {} -> {}", ConfigUtil->local_id, newSessionId);
    ConfigUtil->replaceLocalId(newSessionId);
    if (m_localIdEdit)
        m_localIdEdit->setText(ConfigUtil->local_id);
    emit resetWsCliUrl(buildWsUrl());
}

QString MainWindow::localizedErrorMessage(const QString &message) const
{
    if (message == Constant::ERROR_PASSWORD_INCORRECT ||
        message == QStringLiteral("password_incorrect") ||
        message == QStringLiteral("remote password incorrect") ||
        message == QStringLiteral("Remote password is incorrect"))
    {
        return tr("远端验证码错误");
    }

    if (message == QStringLiteral("The controlled end may not be online") ||
        message == QStringLiteral("controlled_offline"))
    {
        return tr("被控端可能已离线");
    }

    if (message == QStringLiteral("not found recv id"))
    {
        return tr("未找到目标设备");
    }

    if (message == QStringLiteral("Invalid message format"))
    {
        return tr("信令格式错误");
    }

    return message;
}

void MainWindow::connFileMgr(const QString &remote_id, const QString &remote_pwd_md5)
{
    if (!ConfigUtil->showUI)
        return;

    FileTransferWindow *fw = new FileTransferWindow(remote_id, remote_pwd_md5, &m_ws);
    fw->showMaximized();
}

void MainWindow::connDesktopMgr(const QString &remote_id, const QString &remote_pwd_md5)
{
    if (!ConfigUtil->showUI)
        return;

    ControlWindow *cw = new ControlWindow(remote_id, remote_pwd_md5, &m_ws);
    cw->show();
}

void MainWindow::connTerminalMgr(const QString &remote_id, const QString &remote_pwd_md5)
{
    if (!ConfigUtil->showUI)
        return;

    TerminalWindow *tw = new TerminalWindow(remote_id, remote_pwd_md5, &m_ws);
    tw->show();
}

void MainWindow::on_btn_conn_clicked()
{
    QString remote_id = m_remoteIdEdit->text().trimmed();
    QString remote_pwd = m_remotePwdEdit->text().trimmed();
    if (m_remoteIdEdit->text() != remote_id)
        m_remoteIdEdit->setText(remote_id);
    if (m_remotePwdEdit->text() != remote_pwd)
        m_remotePwdEdit->setText(remote_pwd);

    if (remote_id.isEmpty() || remote_pwd.isEmpty())
    {
        LOG_ERROR("错误,远端识别码和密码不能为空");
        if (ConfigUtil->showUI)
        {
            QMessageBox::critical(this, tr("错误"), tr("远端识别码和密码不能为空"));
        }
        return;
    }
    QByteArray hashResult = QCryptographicHash::hash(remote_pwd.toUtf8(), QCryptographicHash::Md5);
    QString remote_pwd_md5 = hashResult.toHex().toUpper();

    if (m_remoteDesktopRadio->isChecked())
    {
        connDesktopMgr(remote_id, remote_pwd_md5);
    }
    else if (m_remoteFileRadio->isChecked())
    {
        connFileMgr(remote_id, remote_pwd_md5);
    }
    else if (m_remoteTerminalRadio->isChecked())
    {
        connTerminalMgr(remote_id, remote_pwd_md5);
    }
}

void MainWindow::on_local_pwd_change_clicked()
{
    ConfigUtil->setLocalPwd(QUuid::createUuid().toString().remove("{").remove("}").toUpper());
    m_localPwdEdit->setText(ConfigUtil->getLocalPwd());
}

void MainWindow::on_local_share_clicked()
{
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(textToCopy.arg(windowTitle, ConfigUtil->local_id, ConfigUtil->getLocalPwd()));
}

void MainWindow::onWsCliConnected()
{
    LOG_INFO("websocket connected");
    if (m_wsConnectStatus)
        m_wsConnectStatus->setText(tr("服务端已连接"));
}

void MainWindow::onWsCliDisconnected()
{
    LOG_WARN("WebSocket disconnected, auto-reconnect will be handled by WsCli");
    if (m_wsConnectStatus)
        m_wsConnectStatus->setText(tr("服务端已断开连接，正在尝试重连..."));
}

void MainWindow::onWsCliReconnectStatus(const QString &status, int phase, int attempt, int nextDelaySeconds)
{
    QString displayStatus;
    if (phase == 0 && attempt == 0)
    {
        displayStatus = tr("服务端已连接");
    }
    else if (nextDelaySeconds > 0)
    {
        displayStatus = tr("服务端断开连接，正在尝试重连... (%1)").arg(status);
    }
    else
    {
        displayStatus = tr("服务端断开连接，重连失败: %1").arg(status);
    }

    if (m_wsConnectStatus)
        m_wsConnectStatus->setText(displayStatus);

    LOG_INFO("Reconnect status update - Phase: {}, Attempt: {}, Status: {}",
             phase, attempt, status);
}

void MainWindow::onWsCliRecvTextMsg(const QString &message)
{
    onWsCliRecvBinaryMsg(message.toUtf8());
}

void MainWindow::onWsCliRecvBinaryMsg(const QByteArray &message)
{
    QJsonObject object = JsonUtil::safeParseObject(message);
    if (!JsonUtil::isValidObject(object))
    {
        LOG_ERROR("Failed to parse JSON in main window");
        return;
    }

    QString sender = JsonUtil::getString(object, Constant::KEY_SENDER);
    QString type = JsonUtil::getString(object, Constant::KEY_TYPE);

    if (sender.isEmpty() || type.isEmpty())
    {
        LOG_ERROR("Missing sender or type in message");
        return;
    }
    if (sender == Constant::ROLE_SERVER)
    {
        if (type == Constant::TYPE_DEVICE_ID_CONFLICT)
        {
            handleDeviceIdConflict(object);
        }
        else if (type == Constant::TYPE_ERROR)
        {
            QString data = JsonUtil::getString(object, Constant::KEY_DATA);
            if (data.isEmpty())
            {
                LOG_ERROR("参数错误,缺少data");
                return;
            }

            LOG_ERROR("错误: " + data);
            if (ConfigUtil->showUI)
            {
                QMessageBox::critical(nullptr, tr("错误"), localizedErrorMessage(data));
            }
        }
    }
    else if (type == Constant::TYPE_CONNECT)
    {
        const QString receiver = JsonUtil::getString(object, Constant::KEY_RECEIVER);
        if (receiver != ConfigUtil->local_id)
        {
            LOG_TRACE("Ignore CONNECT for unrelated receiver: {}", receiver);
            return;
        }

        QString receiverPwd = JsonUtil::getString(object, Constant::KEY_RECEIVER_PWD, "");
        if (receiverPwd.isEmpty() || receiverPwd != ConfigUtil->local_pwd_md5)
        {
            LOG_WARN("Rejected CONNECT from {} because receiver password is invalid", sender);
            QJsonObject errorMsg = JsonUtil::createObject()
                                       .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                                       .add(Constant::KEY_TYPE, Constant::TYPE_ERROR)
                                       .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                                       .add(Constant::KEY_RECEIVER, sender)
                                       .add(Constant::KEY_DATA, Constant::ERROR_PASSWORD_INCORRECT)
                                       .build();
            emit sendWsCliTextMsg(JsonUtil::toCompactString(errorMsg));
            return;
        }
        int fps = JsonUtil::getInt(object, Constant::KEY_FPS, 25);
        bool isOnlyFile = JsonUtil::getBool(object, Constant::KEY_IS_ONLY_FILE, false);
        const int requestedWidth = JsonUtil::getInt(object, Constant::KEY_WIDTH, -1);
        const int requestedHeight = JsonUtil::getInt(object, Constant::KEY_HEIGHT, -1);
        const QString qualityMode = JsonUtil::getString(object, Constant::KEY_QUALITY, QStringLiteral("quality"));
        const QString bitrateProfile = JsonUtil::getString(object, Constant::KEY_BITRATE_PROFILE, QStringLiteral("medium"));
        const QString networkPath = JsonUtil::getString(object, Constant::KEY_NETWORK_PATH, QStringLiteral("auto"));

        LOG_INFO("Received connection request; initial desktop constraint {}x{}, quality={}, bitrateProfile={}, networkPath={}",
                 requestedWidth, requestedHeight, qualityMode, bitrateProfile, networkPath);

        QThread *m_rtc_cli_thread = new QThread();
        QString senderName = QString("WebRtcCli_%1_%2").arg(sender, isOnlyFile ? "file" : "desktop");
        m_rtc_cli_thread->setObjectName(senderName);
        WebRtcCli *m_rtc_cli = new WebRtcCli(sender, fps, isOnlyFile, requestedWidth, requestedHeight,
                                             qualityMode, bitrateProfile, networkPath);

        connect(&m_ws, &WsCli::onWsCliRecvBinaryMsg, m_rtc_cli, &WebRtcCli::onWsCliRecvBinaryMsg);
        connect(&m_ws, &WsCli::onWsCliRecvTextMsg, m_rtc_cli, &WebRtcCli::onWsCliRecvTextMsg);
        connect(m_rtc_cli, &WebRtcCli::sendWsCliBinaryMsg, &m_ws, &WsCli::sendWsCliBinaryMsg);
        connect(m_rtc_cli, &WebRtcCli::sendWsCliTextMsg, &m_ws, &WsCli::sendWsCliTextMsg);

        /* 使用 QPointer 安全管理对象，避免在 WebSocket 断开连接时访问已删除对象 */
        connect(m_rtc_cli, &WebRtcCli::destroyCli, this, &MainWindow::onDestroyWebRtcCli);
        m_rtc_cli->moveToThread(m_rtc_cli_thread);
        m_rtc_cli_thread->start();
        m_rtcCliSessions.insert(m_rtc_cli, m_rtc_cli_thread);
        QMetaObject::invokeMethod(m_rtc_cli, "setDesktopLocked", Qt::QueuedConnection, Q_ARG(bool, m_desktopLocked));
        QMetaObject::invokeMethod(m_rtc_cli, "init", Qt::QueuedConnection);
    }
    else if (type == Constant::TYPE_ERROR)
    {
        const QString receiver = JsonUtil::getString(object, Constant::KEY_RECEIVER);
        if (!receiver.isEmpty() && receiver != ConfigUtil->local_id)
        {
            LOG_TRACE("Ignore peer error for unrelated receiver: {}", receiver);
            return;
        }

        QString data = JsonUtil::getString(object, Constant::KEY_DATA);
        if (data.isEmpty())
        {
            LOG_ERROR("Received error message without data");
            return;
        }

        LOG_ERROR("Peer error: {}", data);
        if (ConfigUtil->showUI)
        {
            QMessageBox::critical(nullptr, tr("错误"), localizedErrorMessage(data));
        }
    }
}

void MainWindow::onDestroyWebRtcCli()
{
    WebRtcCli *webrtc_cli = static_cast<WebRtcCli *>(sender());
    if (webrtc_cli == nullptr)
    {
        LOG_ERROR("webrtc_cli is nullptr in onDestroyWebRtcCli");
        return;
    }

    QThread *m_rtc_cli_thread = m_rtcCliSessions.value(webrtc_cli, webrtc_cli->thread());
    QString senderName = m_rtc_cli_thread ? m_rtc_cli_thread->objectName() : QString("unknown");
    LOG_INFO("Starting destroyCli for {}", senderName);

    m_rtcCliSessions.remove(webrtc_cli);

    if (m_rtc_cli_thread && m_rtc_cli_thread->isRunning())
    {
        QMetaObject::invokeMethod(webrtc_cli, "destroy", Qt::BlockingQueuedConnection);
        webrtc_cli->disconnect();
        QMetaObject::invokeMethod(webrtc_cli, "deleteLater", Qt::BlockingQueuedConnection);
        STOP_PTR_THREAD(m_rtc_cli_thread);
    }
    else
    {
        DELETE_PTR_FUNC(webrtc_cli);
    }

    if (m_rtc_cli_thread)
    {
        delete m_rtc_cli_thread;
    }
    LOG_INFO("Finished destroyCli for {}", senderName);
}
