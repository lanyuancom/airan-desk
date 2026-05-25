/* Split from webrtc_cli.cpp by client-side responsibility. */

#include "webrtc_cli.h"
#include "../common/constant.h"
#include "../util/json_util.h"

void WebRtcCli::onWsCliRecvBinaryMsg(const QByteArray &message)
{
    parseWsMsg(JsonUtil::safeParseObject(message));
}

void WebRtcCli::onWsCliRecvTextMsg(const QString &message)
{
    parseWsMsg(JsonUtil::safeParseObject(message.toUtf8()));
}

/* 简化实现的存根方法 */
void WebRtcCli::parseWsMsg(const QJsonObject &object)
{
    QString type = JsonUtil::getString(object, Constant::KEY_TYPE);
    if (type.isEmpty())
    {
        LOG_ERROR("parseWsMsg: Missing or empty message type");
        return;
    }

    const QString role = JsonUtil::getString(object, Constant::KEY_ROLE);
    if (role != Constant::ROLE_CTL)
    {
        return;
    }

    const QString sender = JsonUtil::getString(object, Constant::KEY_SENDER);
    const QString receiver = JsonUtil::getString(object, Constant::KEY_RECEIVER);
    if (sender != m_remoteId || receiver != ConfigUtil->local_id)
    {
        LOG_TRACE("Ignore signaling message {} for unrelated session sender={}, receiver={}",
                  type, sender, receiver);
        return;
    }

    /* 处理来自控制端的信令消息 */
    if (type == Constant::TYPE_OFFER || type == Constant::TYPE_ANSWER)
    {
        QString data = JsonUtil::getString(object, Constant::KEY_DATA);
        if (!data.isEmpty())
        {
            setRemoteDescription(data, type);
            LOG_TRACE("parseWsMsg: Processed {} message", type);
        }
        else
        {
            LOG_ERROR("parseWsMsg: Empty data for {} message", type);
        }
    }
    else if (type == Constant::TYPE_CANDIDATE)
    {
        QString data = JsonUtil::getString(object, Constant::KEY_DATA);
        QString mid = JsonUtil::getString(object, Constant::KEY_MID);
        if (!data.isEmpty())
        {
            addRemoteCandidateOrQueue(data, mid);
            LOG_TRACE("parseWsMsg: Processed candidate message");
        }
        else
        {
            LOG_ERROR("parseWsMsg: Empty data for candidate message");
        }
    }
}
