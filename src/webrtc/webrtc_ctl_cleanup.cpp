/* Split from webrtc_ctl.cpp by WebRTC control-side responsibility. */

#include "webrtc_ctl.h"
#include "../util/file_packet_util.h"

#include <QMutexLocker>

void WebRtcCtl::destroy()
{
    LOG_DEBUG("WebRtcCtl destroy started");
    m_connected = false;
    m_remoteDescriptionSet = false;
    m_pendingRemoteCandidates.clear();
    m_lastKeyframeRequestTime = {};
    m_keyframeRequestBackoffMs = 1000;
    m_waitingForVideoKeyframe.store(true);
    m_videoKeyframeWaitStartedMs = 0;
    m_lastVideoAdaptFeedbackMs = 0;
    m_lastVideoDecodedMs = 0;
    m_lastVideoPacketMs = 0;
    m_firstVideoPacketWithoutDecodeMs = 0;
    m_lastVideoWatchdogMs = 0;
    m_videoFeedbackDecodedFrames = 0;
    m_videoFeedbackDroppedFrames = 0;
    m_videoFeedbackKeyframeRequests = 0;
    m_videoArrivalWindowStartMs = 0;
    m_videoArrivalLastMs = 0;
    m_videoArrivalBytes = 0;
    m_videoArrivalFrames = 0;
    m_videoArrivalAvgGapMs = 0.0;
    m_videoArrivalJitterMs = 0.0;
    m_videoArrivalLastTimestampUs = 0;
    m_lastAudioTimestampUs = 0;
    m_hasLastAudioTimestamp = false;
    stopAudioCapture();
    stopAudioPlayback();
    m_audioMode = QStringLiteral("off");
    {
        QMutexLocker locker(&m_videoQueueMutex);
        m_videoFrameQueue.clear();
        m_videoDrainScheduled = false;
    }

    /* 清理文件分包工具 */
    if (m_filePacketUtil)
    {
        m_filePacketUtil = nullptr;
    }

    /* 清理数据通道（按顺序清理） */
    if (m_inputChannel)
    {
        LOG_DEBUG("Cleaning up input channel");
        m_inputChannel->resetCallbacks();
        m_inputChannel->close();
        m_inputChannel = nullptr;
    }

    if (m_videoDataChannel)
    {
        LOG_DEBUG("Cleaning up reliable video data channel");
        m_videoDataChannel->resetCallbacks();
        m_videoDataChannel->close();
        m_videoDataChannel = nullptr;
    }

    if (m_fileChannel)
    {
        LOG_DEBUG("Cleaning up file channel");
        m_fileChannel->resetCallbacks();
        m_fileChannel->close();
        m_fileChannel = nullptr;
    }

    if (m_fileTextChannel)
    {
        LOG_DEBUG("Cleaning up file text channel");
        m_fileTextChannel->resetCallbacks();
        m_fileTextChannel->close();
        m_fileTextChannel = nullptr;
    }

    /* 清理轨道 */
    if (m_audioTrack)
    {
        LOG_DEBUG("Cleaning up audio track");
        m_audioTrack->resetCallbacks();
        m_audioTrack->close();
        m_audioTrack = nullptr;
    }

    if (m_videoTrack)
    {
        LOG_DEBUG("Cleaning up video track");
        m_videoTrack->resetCallbacks();
        m_videoTrack->close();
        m_videoTrack = nullptr;
    }

    /* 最后清理PeerConnection */
    if (m_peerConnection)
    {
        LOG_DEBUG("Cleaning up peer connection");
        m_peerConnection->resetCallbacks();
        m_peerConnection->close();
        m_peerConnection = nullptr;
    }

    LOG_INFO("WebRtcCtl destroyed");
}

/* 计划一次重连（指数退避） */
