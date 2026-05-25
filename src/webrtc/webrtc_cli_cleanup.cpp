/* Split from webrtc_cli.cpp by client-side responsibility. */

#include "webrtc_cli.h"
#include "terminal/terminal_session.h"
#include "../util/file_packet_util.h"

void WebRtcCli::destroy()
{
    disconnect();
    /* 设置销毁标志防止回调执行 */
    m_destroying = true;
    m_connected = false;
    m_channelsReady = false;
    m_remoteDescriptionSet = false;
    m_pendingRemoteCandidates.clear();
    m_lastTimestamp = 0;
    m_hasLastTimestamp = false;
    m_lastAudioTimestampUs = 0;
    m_hasLastAudioTimestamp = false;
    m_videoPacerLastRefillMs = 0;
    m_videoPacerTokens = 0;
    m_videoPacerDroppedFrames = 0;
    m_lastVideoPacerDropLogMs = 0;
    m_lastVideoPacerAdaptMs = 0;
    m_adaptLevel = 0;
    m_stableVideoFeedbacks = 0;
    m_lastVideoAdaptMs = 0;
    m_feedbackArrivalKbpsEwma = 0.0;
    m_feedbackArrivalKbpsVar = 0.0;
    m_feedbackJitterMsEwma = 0.0;
    m_feedbackJitterMsVar = 0.0;
    m_feedbackDecodeQueueEwma = 0.0;
    m_feedbackDecodeQueueVar = 0.0;
    m_feedbackLossRateEwma = 0.0;
    m_feedbackLossRateVar = 0.0;
    if (m_inputChannelRecoverTimer)
        m_inputChannelRecoverTimer->stop();
    if (m_videoDataChannelRecoverTimer)
        m_videoDataChannelRecoverTimer->stop();
    if (m_controlWatchdogTimer)
        m_controlWatchdogTimer->stop();
    if (m_disconnectGraceTimer)
        m_disconnectGraceTimer->stop();
    stopAudioCapture();
    stopAudioPlayback();
    m_audioCaptureEnabled = false;
    m_audioMode = QStringLiteral("off");
    if (m_terminalSession)
    {
        m_terminalSession->stop();
        m_terminalSession->deleteLater();
        m_terminalSession = nullptr;
    }

    /* 在释放前先关闭并重置数据通道回调，避免回调在释放期间被触发或持有strong ref */
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

    if (m_fileChannel)
    {
        try
        {
            m_fileChannel->resetCallbacks();
        }
        catch (...)
        {
        }
        try
        {
            m_fileChannel->close();
        }
        catch (...)
        {
        }
        m_fileChannel.reset();
    }

    if (m_fileTextChannel)
    {
        try
        {
            m_fileTextChannel->resetCallbacks();
        }
        catch (...)
        {
        }
        try
        {
            m_fileTextChannel->close();
        }
        catch (...)
        {
        }
        m_fileTextChannel.reset();
    }

    /* 清理轨道，先重置回调并解除 media handler，避免可能的 shared_ptr 循环引用导致对象无法释放 */
    if (m_videoTrack)
    {
        try
        {
            m_videoTrack->resetCallbacks();
        }
        catch (...)
        {
        }
        try
        {
            m_videoTrack->setMediaHandler(nullptr);
        }
        catch (...)
        {
        }
        try
        {
            m_videoTrack->close();
        }
        catch (...)
        {
        }
        m_videoTrack.reset();
    }

    if (m_audioTrack)
    {
        try
        {
            m_audioTrack->resetCallbacks();
        }
        catch (...)
        {
        }
        try
        {
            m_audioTrack->setMediaHandler(nullptr);
        }
        catch (...)
        {
        }
        try
        {
            m_audioTrack->close();
        }
        catch (...)
        {
        }
        m_audioTrack.reset();
    }

    /* 关闭PeerConnection（在通道和轨道都已关闭/解除后关闭） */
    if (m_peerConnection)
    {
        try
        {
            m_peerConnection->resetCallbacks();
        }
        catch (...)
        {
        }
        try
        {
            m_peerConnection->close();
        }
        catch (...)
        {
        }
        m_peerConnection.reset();
    }

    if (m_filePacketUtil)
    {
        /* QObject 父子会在父对象析构时处理，但显式断开并 deleteLater 可更快释放 */
        m_filePacketUtil->disconnect();
        m_filePacketUtil->deleteLater();
        m_filePacketUtil = nullptr;
    }
    /* 清理分包数据 */
    m_uploadFragments.clear();

    LOG_INFO("WebRtcCli destroyed");
}

/* WebSocket消息处理 */
