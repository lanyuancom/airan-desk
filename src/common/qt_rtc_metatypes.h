#ifndef QT_RTC_METATYPES_H
#define QT_RTC_METATYPES_H

#include <QMetaType>
#include <memory>

#include "rtc/rtc.hpp"

Q_DECLARE_METATYPE(rtc::PeerConnection::GatheringState);
Q_DECLARE_METATYPE(rtc::PeerConnection::State);
Q_DECLARE_METATYPE(rtc::message_variant);
Q_DECLARE_METATYPE(rtc::binary);
Q_DECLARE_METATYPE(std::shared_ptr<rtc::binary>);

#endif /* QT_RTC_METATYPES_H */
