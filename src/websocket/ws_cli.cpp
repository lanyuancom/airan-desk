#include "ws_cli.h"
#include <QMetaEnum>
#include <QPointer>

WsCli::WsCli(QObject *parent)
    : QObject{parent},
      m_heart_interval_ms(30000),
      m_ws(nullptr),
      m_connected(false),
      autoConnect(false),
      m_reconnect_phase(0),
      m_reconnect_count(0)
{
    /* 连接重连定时器 */
    m_reconnect_timer = new QTimer(this);
    m_heart_timer = new QTimer(this);
    connect(this, SIGNAL(startReconnectTimer(int)), m_reconnect_timer, SLOT(start(int)));
    connect(this, SIGNAL(stopReconnectTimer()), m_reconnect_timer, SLOT(stop()));
    connect(m_reconnect_timer, &QTimer::timeout, this, &WsCli::attemptReconnect);
    /* 连接心跳定时器 */
    connect(m_heart_timer, &QTimer::timeout, this, &WsCli::sendHeartMsg);
    connect(this, SIGNAL(startHeartTimer(int)), m_heart_timer, SLOT(start(int)));
    connect(this, SIGNAL(stopHeartTimer()), m_heart_timer, SLOT(stop()));
}

WsCli::~WsCli()
{
    if (!m_shutdownDone && QThread::currentThread() == thread())
        shutdown();
}

void WsCli::shutdown()
{
    if (m_shutdownDone)
        return;

    m_shutdownDone = true;
    emit stopHeartTimer();
    emit stopReconnectTimer();
    m_connected = false;

    if (m_ws)
    {
        try
        {
            m_ws->abort();
            m_ws->close();
        }
        catch (...)
        {
        }

        if (QThread::currentThread() == m_ws->thread())
        {
            delete m_ws;
            m_ws = nullptr;
        }
        else
        {
            /* Qt5.9 不支持 QMetaObject::invokeMethod(QObject*, functor, ...)。 */
            QMetaObject::invokeMethod(m_ws, "deleteLater", Qt::BlockingQueuedConnection);
            m_ws = nullptr;
        }
    }

    disconnect();
}

void WsCli::init(const QString &url, quint64 heart_interval_ms)
{
    m_heart_interval_ms = heart_interval_ms;
    m_url = QUrl(url);
    m_connected = false;
    m_reconnect_phase = 0;
    m_reconnect_count = 0;

    m_ws = new QWebSocket();

    connect(this, &WsCli::wsClose, m_ws, &QWebSocket::close);
    connect(this, SIGNAL(wsOpen(QUrl)), m_ws, SLOT(open(QUrl)));
    connect(this, &WsCli::wsPing, m_ws, &QWebSocket::ping);

    connect(m_ws, &QWebSocket::aboutToClose, this, &WsCli::onWsAboutToClose);
    connect(m_ws, &QWebSocket::binaryMessageReceived, this, &WsCli::onWsBinaryMessageReceived);
    connect(m_ws, &QWebSocket::connected, this, &WsCli::onWsConnected);
    connect(m_ws, &QWebSocket::disconnected, this, &WsCli::onWsDisconnected);
    connect(m_ws, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(onWsError(QAbstractSocket::SocketError)));
    connect(m_ws, &QWebSocket::pong, this, &WsCli::onWsPong);
    connect(m_ws, &QWebSocket::preSharedKeyAuthenticationRequired, this, &WsCli::onWsPreSharedKeyAuthenticationRequired);
    connect(m_ws, &QWebSocket::proxyAuthenticationRequired, this, &WsCli::onWsProxyAuthenticationRequired);
    connect(m_ws, &QWebSocket::sslErrors, this, &WsCli::onWsSslErrors);
    connect(m_ws, &QWebSocket::textMessageReceived, this, &WsCli::onWsTextMessageReceived);

    LOG_INFO("WebSocket connections established, opening connection to: {}", url);
    emit this->wsOpen(m_url);
}

void WsCli::resetUrlAndReconnect(const QString &url)
{
    m_url = QUrl(url);
    m_connected = false;
    m_reconnect_phase = 0;
    m_reconnect_count = 0;
    emit stopHeartTimer();
    emit stopReconnectTimer();

    if (!m_ws)
    {
        init(url, m_heart_interval_ms);
        return;
    }

    LOG_INFO("WebSocket URL updated, reconnecting to: {}", url);
    m_ws->abort();
    m_ws->open(m_url);
}

void WsCli::onWsAboutToClose()
{
    LOG_ERROR("aboutToClose");
    this->m_connected = false;
}

void WsCli::onWsBinaryMessageReceived(const QByteArray &message)
{
    LOG_TRACE("size:{}", message.size());
    emit this->onWsCliRecvBinaryMsg(message);
}

void WsCli::onWsTextMessageReceived(const QString &message)
{
    LOG_TRACE("text size:{}", message.toUtf8().size());
    emit this->onWsCliRecvTextMsg(message);
}

void WsCli::onWsConnected()
{
    LOG_INFO("WebSocket connected successfully");
    this->m_connected = true;

    /* 重置重连状态 */
    m_reconnect_phase = 0;
    m_reconnect_count = 0;
    emit stopReconnectTimer();

    /* 发送连接成功状态更新 */
    emit onReconnectStatusUpdate(tr("连接已恢复"), 0, 0, 0);

    emit startHeartTimer(m_heart_interval_ms);
    emit this->onWsCliConnected();
}

void WsCli::onWsDisconnected()
{
    LOG_WARN("WebSocket disconnected, starting intelligent reconnect");
    this->m_connected = false;
    emit stopHeartTimer();

    emit onWsCliDisconnected();

    if (m_shutdownDone)
    {
        LOG_DEBUG("WebSocket disconnected during shutdown; not scheduling reconnect");
        return;
    }
    /* 启动智能重连 */
    scheduleReconnect();
}

void WsCli::onWsError(QAbstractSocket::SocketError error)
{
    LOG_ERROR("WebSocket error: {} ({})", static_cast<int>(error), QMetaEnum::fromType<QAbstractSocket::SocketError>().valueToKey(error));
    /* 某些错误也可能需要重连 */
    if (!m_connected && error != QAbstractSocket::OperationError)
    {
        LOG_INFO("Error occurred, may need to reconnect");
    }
}

void WsCli::onWsPong(quint64 elapsedTime, const QByteArray &payload)
{
    LOG_TRACE("pong elapsedTime: {} payloadSize: {}", elapsedTime, payload.size());
}

void WsCli::onWsPreSharedKeyAuthenticationRequired(QSslPreSharedKeyAuthenticator *authenticator)
{
    Q_UNUSED(authenticator);
    LOG_ERROR("onWsPreSharedKeyAuthenticationRequired");
}

void WsCli::onWsProxyAuthenticationRequired(const QNetworkProxy &proxy, QAuthenticator *authenticator)
{
    Q_UNUSED(proxy);
    Q_UNUSED(authenticator);
    LOG_ERROR("onWsProxyAuthenticationRequired");
}

void WsCli::onWsSslErrors(const QList<QSslError> &errors)
{
    Q_UNUSED(errors);
    LOG_ERROR("onWsSslErrors");
}

void WsCli::reConnect()
{
    emit this->wsOpen(m_url);
}

void WsCli::sendWsCliTextMsg(const QString &msg)
{
    if (!m_ws)
    {
        LOG_WARN("Cannot send WebSocket text message because socket is null");
        return;
    }
    m_ws->sendTextMessage(msg);
    m_ws->flush();
}

void WsCli::sendWsCliBinaryMsg(const QByteArray &msg)
{
    if (!m_ws)
    {
        LOG_WARN("Cannot send WebSocket binary message because socket is null");
        return;
    }
    m_ws->sendBinaryMessage(msg);
    m_ws->flush();
}

void WsCli::sendHeartMsg()
{
    if (m_connected && m_ws)
    {
        m_ws->sendTextMessage("@heart");
        m_ws->flush();
    }
}

void WsCli::scheduleReconnect()
{
    if (m_shutdownDone)
    {
        LOG_DEBUG("Shutdown in progress, skipping reconnect");
        return;
    }
    if (m_connected)
    {
        LOG_DEBUG("Already connected, no need to reconnect");
        return; /* 已连接，无需重连 */
    }

    int delay = 1000; /* 默认1秒 */
    QString phaseDesc;

    switch (m_reconnect_phase)
    {
    case 0: /* 1秒重试阶段 */
        delay = 1000;
        phaseDesc = tr("快速重连");
        break;
    case 1: /* 10秒重试阶段 */
        delay = 10000;
        phaseDesc = tr("中速重连");
        break;
    case 2: /* 30秒重试阶段 */
        delay = 30000;
        phaseDesc = tr("慢速重连");
        break;
    case 3: /* 60秒重试阶段（永久） */
        delay = 60000;
        phaseDesc = tr("长期重连");
        break;
    }

    LOG_INFO("Scheduling reconnect in {}ms (phase: {}, attempt: {})",
             delay, m_reconnect_phase, m_reconnect_count + 1);

    /* 发送状态更新信号 */
    QString status = tr("%1阶段，%2秒后重连...").arg(phaseDesc).arg(delay / 1000);
    emit onReconnectStatusUpdate(status, m_reconnect_phase, m_reconnect_count + 1, delay / 1000);

    emit startReconnectTimer(delay);
}

void WsCli::attemptReconnect()
{
    LOG_INFO("attemptReconnect() called");

    if (m_shutdownDone)
    {
        LOG_INFO("Shutdown in progress, aborting reconnect attempt");
        return;
    }
    if (m_connected)
    {
        LOG_INFO("Already connected, stopping reconnect attempts");
        return; /* 已连接，停止重连 */
    }

    m_reconnect_count++;

    LOG_INFO("Attempting reconnect (phase: {}, attempt: {})", m_reconnect_phase, m_reconnect_count);

    /* 发送正在重连的状态更新 */
    QString status = tr("正在尝试重连... (第%1次)").arg(m_reconnect_count);
    emit onReconnectStatusUpdate(status, m_reconnect_phase, m_reconnect_count, 0);

    /* 尝试重连 */
    if (m_ws)
    {
        LOG_INFO("Calling m_ws->open() to reconnect");
        m_ws->open(m_url);
    }
    else
    {
        LOG_ERROR("m_ws is null, cannot reconnect");
        return;
    }

    /* 检查是否需要进入下一个重连阶段 */
    if (m_reconnect_count >= MAX_RETRY_PER_PHASE && m_reconnect_phase < 3)
    {
        m_reconnect_phase++;
        m_reconnect_count = 0;
        LOG_INFO("Moving to reconnect phase {}", m_reconnect_phase);
    }
    else if (m_reconnect_phase == 3)
    {
        /* 第四阶段（60秒）永久重试，重置计数但不改变阶段 */
        if (m_reconnect_count >= MAX_RETRY_PER_PHASE)
        {
            m_reconnect_count = 0;
            LOG_INFO("Phase 3: Resetting retry count for continuous attempts");
        }
    }

    /* 等待一小段时间再调度下一次重连（给连接一点时间） */
    QTimer::singleShot(2000, this, &WsCli::scheduleNextReconnectIfNeeded);
}

void WsCli::scheduleNextReconnectIfNeeded()
{
    LOG_DEBUG("Checking if need to schedule next reconnect");
    if (m_shutdownDone)
    {
        LOG_DEBUG("Shutdown in progress, skipping next reconnect");
        return;
    }
    if (!m_connected)
    {
        LOG_INFO("Still not connected, scheduling next reconnect");
        scheduleReconnect();
    }
    else
    {
        LOG_INFO("Connected successfully, stopping reconnect attempts");
    }
}
