#ifndef OPUS_DUPLEX_MEDIA_HANDLER_H
#define OPUS_DUPLEX_MEDIA_HANDLER_H

#include "rtc/rtc.hpp"

class OpusDuplexMediaHandler final : public rtc::MediaHandler
{
public:
    OpusDuplexMediaHandler(uint32_t ssrc, const std::string &cname, uint8_t payloadType);

    void media(const rtc::Description::Media &desc) override;
    void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override;
    void outgoing(rtc::message_vector &messages, const rtc::message_callback &send) override;

private:
    std::shared_ptr<rtc::RtpDepacketizer> m_depacketizer;
    std::shared_ptr<rtc::OpusRtpPacketizer> m_packetizer;
};

#endif /* OPUS_DUPLEX_MEDIA_HANDLER_H */
