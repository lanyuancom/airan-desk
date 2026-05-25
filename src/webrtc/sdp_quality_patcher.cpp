#include "sdp_quality_patcher.h"

#include "../codec/h264_profile_level.h"

#include <QSet>

namespace
{
const QString kOpusFmtpHighQuality = QStringLiteral("minptime=10;useinbandfec=1;usedtx=0;stereo=1;sprop-stereo=1;maxplaybackrate=48000;maxaveragebitrate=48000");

QString sdpPayloadType(const QString &line, const QString &prefix)
{
    if (!line.startsWith(prefix))
        return QString();

    const int start = prefix.size();
    int end = line.indexOf(' ', start);
    if (end < 0)
        end = line.size();
    return line.mid(start, end - start).trimmed();
}

QString fmtpParamValue(const QString &params, const QString &key)
{
    const QStringList parts = params.split(QChar(';'));
    const QString lowerKey = key.toLower();
    for (const QString &part : parts)
    {
        const QString item = part.trimmed();
        const int eq = item.indexOf(QChar('='));
        if (eq <= 0)
            continue;
        if (item.left(eq).trimmed().toLower() == lowerKey)
            return item.mid(eq + 1).trimmed();
    }
    return QString();
}

QString rtxSsrcInFidLine(const QString &line)
{
    const QStringList parts = line.simplified().split(QChar(' '));
    return parts.size() >= 3 && parts.at(0).compare(QStringLiteral("a=ssrc-group:FID"), Qt::CaseInsensitive) == 0
               ? parts.at(2)
               : QString();
}

QString ssrcFromAttributeLine(const QString &line)
{
    if (!line.startsWith(QStringLiteral("a=ssrc:")))
        return QString();
    const int start = QStringLiteral("a=ssrc:").size();
    int end = line.indexOf(QChar(' '), start);
    if (end < 0)
        end = line.indexOf(QChar(':'), start);
    if (end < 0)
        end = line.size();
    return line.mid(start, end - start).trimmed();
}

QString removeFmtpParam(const QString &params, const QString &key)
{
    const QStringList parts = params.split(QChar(';'));
    QStringList kept;
    const QString lowerKey = key.toLower();
    for (const QString &part : parts)
    {
        const QString item = part.trimmed();
        if (item.isEmpty() || item.toLower().startsWith(lowerKey + QStringLiteral("=")))
            continue;
        kept.append(item);
    }
    return kept.join(QStringLiteral(";"));
}

QStringList patchH264VideoSection(const QStringList &section, const QString &profileLevelId, bool &changed)
{
    QString selectedH264Payload;
    QSet<QString> rejectedPayloads;
    QSet<QString> rejectedSsrcs;
    bool hasSelectedFmtp = false;
    for (const QString &line : section)
    {
        const QString lower = line.toLower();
        if (lower.startsWith(QStringLiteral("a=rtpmap:")) && lower.contains(QStringLiteral(" h264/90000")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=rtpmap:"));
            if (!pt.isEmpty() && selectedH264Payload.isEmpty())
                selectedH264Payload = pt;
        }
        else if (lower.startsWith(QStringLiteral("a=fmtp:")) && !selectedH264Payload.isEmpty())
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=fmtp:"));
            if (pt == selectedH264Payload)
                hasSelectedFmtp = true;
        }
    }

    if (selectedH264Payload.isEmpty())
        return section;

    for (const QString &line : section)
    {
        const QString lower = line.toLower();
        if (lower.startsWith(QStringLiteral("a=rtpmap:")) ||
            lower.startsWith(QStringLiteral("a=fmtp:")) ||
            lower.startsWith(QStringLiteral("a=rtcp-fb:")))
        {
            const QString pt = sdpPayloadType(line, line.left(line.indexOf(QChar(':')) + 1));
            if (!pt.isEmpty() && pt != selectedH264Payload)
                rejectedPayloads.insert(pt);
        }
        if (lower.startsWith(QStringLiteral("a=fmtp:")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=fmtp:"));
            const int space = line.indexOf(QChar(' '));
            const QString apt = space >= 0 ? fmtpParamValue(line.mid(space + 1), QStringLiteral("apt")) : QString();
            if (!pt.isEmpty() && rejectedPayloads.contains(pt) && apt == selectedH264Payload)
                rejectedPayloads.insert(pt);
        }
        if (lower.startsWith(QStringLiteral("a=ssrc-group:fid")))
        {
            const QString rtxSsrc = rtxSsrcInFidLine(line);
            if (!rtxSsrc.isEmpty())
                rejectedSsrcs.insert(rtxSsrc);
        }
    }

    QStringList out;
    for (const QString &line : section)
    {
        const QString lower = line.toLower();
        const QString mediaPayload = lower.startsWith(QStringLiteral("a=rtpmap:")) ||
                                             lower.startsWith(QStringLiteral("a=fmtp:")) ||
                                             lower.startsWith(QStringLiteral("a=rtcp-fb:"))
                                         ? sdpPayloadType(line, line.left(line.indexOf(QChar(':')) + 1))
                                         : QString();

        if (lower.startsWith(QStringLiteral("m=video")))
        {
            const QStringList parts = line.split(QChar(' '));
            QStringList kept;
            const int fixedPartCount = qMin(3, parts.size());
            for (int i = 0; i < fixedPartCount; ++i)
                kept.append(parts.at(i));
            kept.append(selectedH264Payload);
            const QString patched = kept.join(QStringLiteral(" "));
            out.append(patched);
            changed |= patched != line;
            continue;
        }

        if (!mediaPayload.isEmpty() && mediaPayload != selectedH264Payload)
        {
            changed = true;
            continue;
        }

        if (lower.startsWith(QStringLiteral("a=ssrc-group:fid")))
        {
            changed = true;
            continue;
        }

        const QString ssrc = ssrcFromAttributeLine(line);
        if (!ssrc.isEmpty() && rejectedSsrcs.contains(ssrc))
        {
            changed = true;
            continue;
        }

        if (lower.startsWith(QStringLiteral("a=fmtp:")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=fmtp:"));
            if (pt == selectedH264Payload)
            {
                out.append(QStringLiteral("a=fmtp:%1 profile-level-id=%2;level-asymmetry-allowed=1;packetization-mode=1")
                               .arg(pt, profileLevelId));
                changed = true;
                continue;
            }
        }

        out.append(line);
        if (lower.startsWith(QStringLiteral("a=rtpmap:")) && lower.contains(QStringLiteral(" h264/90000")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=rtpmap:"));
            if (pt == selectedH264Payload && !hasSelectedFmtp)
            {
                out.append(QStringLiteral("a=fmtp:%1 profile-level-id=%2;level-asymmetry-allowed=1;packetization-mode=1")
                               .arg(pt, profileLevelId));
                changed = true;
            }
        }
    }
    return out;
}

QString patchOpusFmtpLine(const QString &line)
{
    const int space = line.indexOf(QChar(' '));
    if (space < 0)
        return line + QLatin1Char(' ') + kOpusFmtpHighQuality;

    QString params = line.mid(space + 1);
    params = removeFmtpParam(params, QStringLiteral("minptime"));
    params = removeFmtpParam(params, QStringLiteral("useinbandfec"));
    params = removeFmtpParam(params, QStringLiteral("usedtx"));
    params = removeFmtpParam(params, QStringLiteral("stereo"));
    params = removeFmtpParam(params, QStringLiteral("sprop-stereo"));
    params = removeFmtpParam(params, QStringLiteral("maxplaybackrate"));
    params = removeFmtpParam(params, QStringLiteral("maxaveragebitrate"));

    const QString prefix = line.left(space + 1);
    return params.isEmpty() ? prefix + kOpusFmtpHighQuality : prefix + params + QLatin1Char(';') + kOpusFmtpHighQuality;
}

QStringList patchOpusAudioSection(const QStringList &section, bool &changed)
{
    QStringList opusPayloads;
    QSet<QString> fmtpPayloads;
    for (const QString &line : section)
    {
        const QString lower = line.toLower();
        if (lower.startsWith(QStringLiteral("a=rtpmap:")) && lower.contains(QStringLiteral(" opus/48000")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=rtpmap:"));
            if (!pt.isEmpty() && !opusPayloads.contains(pt))
                opusPayloads.append(pt);
        }
        else if (lower.startsWith(QStringLiteral("a=fmtp:")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=fmtp:"));
            if (!pt.isEmpty())
                fmtpPayloads.insert(pt);
        }
    }

    if (opusPayloads.isEmpty())
        return section;

    QStringList out;
    for (const QString &line : section)
    {
        const QString lower = line.toLower();
        if (lower.startsWith(QStringLiteral("a=fmtp:")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=fmtp:"));
            if (!pt.isEmpty() && opusPayloads.contains(pt))
            {
                const QString patched = patchOpusFmtpLine(line);
                out.append(patched);
                changed |= patched != line;
                continue;
            }
        }

        out.append(line);
        if (lower.startsWith(QStringLiteral("a=rtpmap:")) && lower.contains(QStringLiteral(" opus/48000")))
        {
            const QString pt = sdpPayloadType(line, QStringLiteral("a=rtpmap:"));
            if (!pt.isEmpty() && !fmtpPayloads.contains(pt))
            {
                out.append(QStringLiteral("a=fmtp:%1 %2").arg(pt, kOpusFmtpHighQuality));
                changed = true;
            }
        }
    }
    return out;
}
} /* namespace */

QString SdpQualityPatcher::apply(QString sdp, int width, int height, int fps)
{
    QString normalized = sdp;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QChar('\r'), QChar('\n'));

    const QString profileLevelId = H264ProfileLevel::constrainedBaselineProfileLevelId(width, height, fps);
    const QStringList lines = normalized.split(QChar('\n'));
    QStringList out;
    bool changed = false;
    int i = 0;
    while (i < lines.size())
    {
        const QString line = lines.at(i);
        if (line.startsWith(QStringLiteral("m=")))
        {
            QStringList section;
            section.append(line);
            ++i;
            while (i < lines.size() && !lines.at(i).startsWith(QStringLiteral("m=")))
            {
                section.append(lines.at(i));
                ++i;
            }

            if (line.startsWith(QStringLiteral("m=video")))
                out.append(patchH264VideoSection(section, profileLevelId, changed));
            else if (line.startsWith(QStringLiteral("m=audio")))
                out.append(patchOpusAudioSection(section, changed));
            else
                out.append(section);
            continue;
        }

        out.append(line);
        ++i;
    }

    return changed ? out.join(QStringLiteral("\r\n")) : sdp;
}
