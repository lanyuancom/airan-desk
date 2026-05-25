/* Split from webrtc_ctl.cpp by WebRTC control-side responsibility. */

#include "webrtc_ctl.h"
#include "../common/constant.h"
#include "../util/json_util.h"

void WebRtcCtl::parseWsMsg(const QJsonObject &object)
{
    /* 忽略非信令消息（如心跳），避免无意义错误日志刷屏 */
    if (!object.contains(Constant::KEY_ROLE) || !object.contains(Constant::KEY_TYPE))
        return;

    QString role = JsonUtil::getString(object, Constant::KEY_ROLE);
    QString type = JsonUtil::getString(object, Constant::KEY_TYPE);

    /* 只处理来自被控端的消息 */
    if (role != Constant::ROLE_CLI)
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

    if (!m_peerConnection)
    {
        LOG_WARN("Ignore signaling message {} because peer connection is not ready", type);
        return;
    }

    /* 处理SDP消息（Offer/Answer） */
    if (type == Constant::TYPE_OFFER || type == Constant::TYPE_ANSWER)
    {
        QString data = JsonUtil::getString(object, Constant::KEY_DATA);
        if (!data.isEmpty())
        {
            try
            {
                LOG_INFO("Setting remote description: {}", type);
                rtc::Description desc(data.toStdString(), type.toStdString());
                m_peerConnection->setRemoteDescription(desc);
                m_remoteDescriptionSet = true;
                flushPendingRemoteCandidates();
                if (type == Constant::TYPE_OFFER)
                {
                    m_peerConnection->createAnswer();
                }
                LOG_INFO("Remote description set successfully");
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("Failed to set remote description: {}", e.what());
            }
            catch (...)
            {
                LOG_ERROR("Failed to set remote description: unknown error");
            }
        }
    }
    /* 处理ICE候选者 */
    else if (type == Constant::TYPE_CANDIDATE)
    {
        QString candidateStr = JsonUtil::getString(object, Constant::KEY_DATA);
        QString mid = JsonUtil::getString(object, Constant::KEY_MID);

        if (!candidateStr.isEmpty() && !mid.isEmpty())
        {
            addRemoteCandidateOrQueue(candidateStr, mid);
        }
    }
}

void WebRtcCtl::addRemoteCandidateOrQueue(const QString &candidate, const QString &mid)
{
    if (!m_peerConnection)
        return;

    if (!m_remoteDescriptionSet)
    {
        m_pendingRemoteCandidates.append(qMakePair(candidate, mid));
        LOG_DEBUG("Queued remote ICE candidate until remote description is set, pending={}", m_pendingRemoteCandidates.size());
        return;
    }

    try
    {
        m_peerConnection->addRemoteCandidate(rtc::Candidate(candidate.toStdString(), mid.toStdString()));
        LOG_DEBUG("Added remote candidate");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to add remote candidate: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Failed to add remote candidate: unknown error");
    }
}

void WebRtcCtl::flushPendingRemoteCandidates()
{
    if (!m_peerConnection || !m_remoteDescriptionSet || m_pendingRemoteCandidates.isEmpty())
        return;

    const QVector<QPair<QString, QString>> pending = m_pendingRemoteCandidates;
    m_pendingRemoteCandidates.clear();
    for (const auto &candidate : pending)
    {
        addRemoteCandidateOrQueue(candidate.first, candidate.second);
    }
}

void WebRtcCtl::onWsCliRecvBinaryMsg(const QByteArray &message)
{
    parseWsMsg(JsonUtil::safeParseObject(message));
}

void WebRtcCtl::onWsCliRecvTextMsg(const QString &message)
{
    parseWsMsg(JsonUtil::safeParseObject(message.toUtf8()));
}

