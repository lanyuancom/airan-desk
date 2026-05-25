/* Split from webrtc_ctl.cpp by WebRTC control-side responsibility. */

#include "webrtc_ctl.h"
#include "../codec/h264_decoder.h"
#include "../util/file_packet_util.h"

#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <algorithm>

void WebRtcCtl::scheduleReconnect()
{
    if (QThread::currentThread() != thread())
    {
        QMetaObject::invokeMethod(this, "scheduleReconnect", Qt::QueuedConnection);
        return;
    }

    if (!m_allowReconnect)
        return;

    /* 多线程回调只允许首个调用者真正进入调度，避免一次断线瞬间排队多次 */
    bool expected = false;
    if (!m_reconnectPending.compare_exchange_strong(expected, true))
    {
        LOG_DEBUG("Reconnect already pending");
        return;
    }

    if (m_reconnectTimer && m_reconnectTimer->isActive())
    {
        LOG_DEBUG("Reconnect already scheduled");
        return;
    }

    m_reconnectAttempts++;

    /* 首次重连更快，后续指数退避；上限缩短到 15 秒，避免长时间无法控制 */
    if (m_reconnectAttempts == 1)
    {
        m_reconnectBackoffMs = 3000;
    }
    else
    {
        m_reconnectBackoffMs = std::min(m_reconnectBackoffMs * 2, 15000);
    }

    LOG_INFO("Scheduling reconnect attempt {}, backoff {} ms", m_reconnectAttempts, m_reconnectBackoffMs);
    /* 必须设为单次触发，避免连接成功后定时器仍循环触发重连 */
    if (m_reconnectTimer)
    {
        m_reconnectTimer->setSingleShot(true);
        m_reconnectTimer->setInterval(m_reconnectBackoffMs);
        m_reconnectTimer->start();
    }
}

void WebRtcCtl::stopReconnect()
{
    if (QThread::currentThread() != thread())
    {
        QMetaObject::invokeMethod(this, "stopReconnect", Qt::QueuedConnection);
        return;
    }

    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    m_reconnectAttempts = 0;
    m_reconnectBackoffMs = 3000; /* 重置退避时间，下次重连从头开始 */
    m_reconnectPending.store(false);
}

void WebRtcCtl::setNetworkPath(const QString &networkPath)
{
    if (QThread::currentThread() != thread())
    {
        QMetaObject::invokeMethod(this, "setNetworkPath", Qt::QueuedConnection,
                                  Q_ARG(QString, networkPath));
        return;
    }

    const QString normalized = networkPath.toLower();
    if (normalized != QStringLiteral("auto") &&
        normalized != QStringLiteral("direct") &&
        normalized != QStringLiteral("turn_udp") &&
        normalized != QStringLiteral("turn_tcp"))
    {
        LOG_WARN("Ignored unknown network path: {}", networkPath);
        return;
    }

    if (m_networkPath == normalized)
    {
        LOG_INFO("Network path unchanged: {}", normalized);
        return;
    }

    m_networkPath = normalized;
    LOG_INFO("Network path switched to {}, rebuilding PeerConnection after remote session drain", m_networkPath);
    publishNetworkPathState();

    m_connected = false;
    m_reconnectPending.store(false);
    if (m_reconnectTimer)
        m_reconnectTimer->stop();

    destroy();
    m_pendingNetworkPathReconnect = m_networkPath;
    QTimer::singleShot(1200, this, &WebRtcCtl::restartAfterNetworkPathChange);
}

void WebRtcCtl::restartAfterNetworkPathChange()
{
    const QString requestedPath = m_pendingNetworkPathReconnect;
    if (m_shutdownDone || m_networkPath != requestedPath)
        return;

    if (!m_filePacketUtil)
    {
        m_filePacketUtil = std::make_unique<FilePacketUtil>(this);
        QObject::connect(m_filePacketUtil.get(), &FilePacketUtil::fileDownloadCompleted,
                         this, &WebRtcCtl::recvDownloadFile);
        QObject::connect(m_filePacketUtil.get(), &FilePacketUtil::fileReceived,
                         this, &WebRtcCtl::recvDownloadFile);
    }

    try
    {
        init();
        LOG_INFO("Network path reconnect started with path={}", m_networkPath);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Network path reconnect failed to start: {}", e.what());
        scheduleReconnect();
    }
    catch (...)
    {
        LOG_ERROR("Network path reconnect failed to start: unknown error");
        scheduleReconnect();
    }
}

/* 重连执行函数（由定时器触发） */
void WebRtcCtl::doReconnect()
{
    /* 定时器已触发，允许后续再次调度 */
    m_reconnectPending.store(false);

    LOG_INFO("Reconnect attempt {} starting", m_reconnectAttempts);

    /* 清理当前连接（但不影响类级工具对象，若被清理则重建） */
    m_connected = false;
    destroy();

    /* 确保文件分包工具在需要时被重建并重新连接信号 */
    if (!m_filePacketUtil)
    {
        m_filePacketUtil = std::make_unique<FilePacketUtil>(this);
        connect(m_filePacketUtil.get(), &FilePacketUtil::fileDownloadCompleted,
                this, &WebRtcCtl::recvDownloadFile);
        connect(m_filePacketUtil.get(), &FilePacketUtil::fileReceived,
                this, &WebRtcCtl::recvDownloadFile);
    }

    /* 重新初始化（会发起新的 CONNECT/SDP 流程） */
    try
    {
        init();
        LOG_INFO("Reconnect attempt {}: init() invoked", m_reconnectAttempts);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Exception during reconnect init: {}", e.what());
        scheduleReconnect();
    }
    catch (...)
    {
        LOG_ERROR("Unknown exception during reconnect init");
        scheduleReconnect();
    }
}

/* 处理接收到的音频数据 */
