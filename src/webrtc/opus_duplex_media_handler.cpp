#include "opus_duplex_media_handler.h"

OpusDuplexMediaHandler::OpusDuplexMediaHandler(uint32_t ssrc, const std::string &cname, uint8_t payloadType)
    : m_depacketizer(std::make_shared<rtc::RtpDepacketizer>(rtc::OpusRtpPacketizer::DefaultClockRate))
{
    auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, payloadType, rtc::OpusRtpPacketizer::DefaultClockRate);
    m_packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
    m_packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfig));
}

void OpusDuplexMediaHandler::media(const rtc::Description::Media &desc)
{
    m_depacketizer->mediaChain(desc);
    m_packetizer->mediaChain(desc);
}

void OpusDuplexMediaHandler::incoming(rtc::message_vector &messages, const rtc::message_callback &send)
{
    m_depacketizer->incomingChain(messages, send);
}

void OpusDuplexMediaHandler::outgoing(rtc::message_vector &messages, const rtc::message_callback &send)
{
    m_packetizer->outgoingChain(messages, send);
}
