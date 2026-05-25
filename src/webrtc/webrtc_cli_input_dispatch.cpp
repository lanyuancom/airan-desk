/* Split from webrtc_cli.cpp by client-side responsibility. */

#include "webrtc_cli.h"
#include "../common/constant.h"
#include "../desktop/desktop_capture_manager.h"
#include "../util/input_util.h"
#include "../util/json_util.h"

#include <QDateTime>

void WebRtcCli::parseInputMsg(const QJsonObject &object)
{
    QString msgType = JsonUtil::getString(object, Constant::KEY_MSGTYPE);
    if (msgType.isEmpty())
    {
        LOG_ERROR("parseInputMsg: Missing msgType");
        return;
    }
    QString senderId = JsonUtil::getString(object, Constant::KEY_SENDER);
    if (senderId.isEmpty() || senderId != m_remoteId)
    {
        LOG_WARNING("parseInputMsg: Ignoring message from unknown sender: {}", senderId);
        return;
    }
    QString remoteId = JsonUtil::getString(object, Constant::KEY_RECEIVER);
    QString remotePwd = JsonUtil::getString(object, Constant::KEY_RECEIVER_PWD);
    if (remoteId.isEmpty() || remoteId != ConfigUtil->local_id || remotePwd != ConfigUtil->local_pwd_md5)
    {
        LOG_WARNING("parseInputMsg: Ignoring message for receiver {}, expected {}, passwordValid={}",
                    remoteId, ConfigUtil->local_id, remotePwd == ConfigUtil->local_pwd_md5);
        return;
    }
    m_lastControlAliveMs = QDateTime::currentMSecsSinceEpoch();
    if (msgType == Constant::TYPE_MOUSE)
    {
        /* 处理鼠标事件 */
        handleMouseEvent(object);
    }
    else if (msgType == Constant::TYPE_KEYBOARD)
    {
        /* 处理键盘事件 */
        handleKeyboardEvent(object);
    }
    else if (msgType == Constant::TYPE_STREAM_CONFIG)
    {
        handleStreamConfig(object);
    }
    else if (msgType == Constant::TYPE_VIDEO_ADAPT_FEEDBACK)
    {
        handleVideoAdaptFeedback(object);
    }
    else if (msgType == Constant::TYPE_AUDIO_CAPTURE)
    {
        handleAudioCaptureConfig(object);
    }
    else if (msgType == Constant::TYPE_SWITCH_SCREEN)
    {
        handleSwitchScreen(object);
    }
    else if (msgType == Constant::TYPE_KEYFRAME_REQUEST)
    {
        if (DesktopCaptureManager::instance()->requestKeyframe(m_subscriberId, m_screenIndex))
        {
            LOG_INFO("Received keyframe request from control side");
        }
    }
    else if (msgType == Constant::TYPE_CONTROL_HEARTBEAT)
    {
        LOG_TRACE("Control heartbeat received from {}", senderId);
    }
    else if (msgType == Constant::TYPE_REMOTE_OPERATION)
    {
        handleRemoteOperation(object);
    }
    else if (msgType == Constant::TYPE_ANDROID_NAVIGATION)
    {
        QString errorMessage;
        const QString action = JsonUtil::getString(object, Constant::KEY_ACTION);
        const bool ok = InputUtil::execAndroidNavigation(action, &errorMessage);
        if (!ok)
            LOG_WARN("Android navigation failed: action={}, error={}", action, errorMessage);
    }
    else
    {
        LOG_WARNING("parseInputMsg: Unknown input message type: {}", msgType);
    }
}
