#ifndef SDP_QUALITY_PATCHER_H
#define SDP_QUALITY_PATCHER_H

#include <QString>

namespace SdpQualityPatcher
{
QString apply(QString sdp, int width = 0, int height = 0, int fps = 0);
}

#endif /* SDP_QUALITY_PATCHER_H */
