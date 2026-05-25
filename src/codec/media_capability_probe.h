#ifndef MEDIA_CAPABILITY_PROBE_H
#define MEDIA_CAPABILITY_PROBE_H

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <memory>

#include <rtc/common.hpp>

class MediaCapabilityProbe
{
public:
    enum class PathStatus
    {
        Disabled,
        Available,
        Stable,
        Experimental
    };

    struct ImplementationStatus
    {
        QString name;
        QString implementation;
        QString implType;
        bool isHardware{false};
        bool openable{false};
        int priority{0};
        PathStatus status{PathStatus::Disabled};
        int framesTested{0};
        int stableFrames{0};
        QJsonArray warnings;

        QJsonObject toJson() const;
    };

    struct PathProbeStatus
    {
        QString name;
        QString encodeImpl;
        QString decodeImpl;
        bool zeroCopy{false};
        PathStatus status{PathStatus::Disabled};
        int framesTested{0};
        int stableFrames{0};
        QJsonArray warnings;

        QJsonObject toJson() const;
    };

    struct ProbeResult
    {
        bool encoderAvailable{false};
        bool decoderAvailable{false};
        bool loopbackStable{false};
        QString encoderName;
        QString decoderName;
        QString recommendedProfile{"baseline"};
        QString codec{"h264"};
        int width{1280};
        int height{720};
        int fps{20};
        int bitrate{3500000};
        bool zeroCopy{false};
        QString packetizationMode{"1"};
        QString profileLevelId{"42e01f"};
        bool allowHighPerformance{true};
        QJsonArray warnings;
        QJsonArray codecs;
        QJsonObject encode;
        QJsonObject decode;
        QJsonArray paths;
        QJsonObject maxStable;
        QJsonObject recommended;
        QJsonObject recommendedSend;
        QJsonObject recommendedReceive;
        QJsonArray supportedCodecs;
        QJsonArray supportedProfiles;
        QJsonArray implementations;

        QJsonObject toJson() const;
    };

    static ProbeResult run(int width, int height, int fps);

private:
    static QString pathStatusToString(PathStatus status);
    static QJsonObject buildRecommended(const ProbeResult &result, const QString &directionHint);
    static ImplementationStatus buildEncoderStatus(int width, int height, int fps, const QImage &frame);
    static ImplementationStatus buildDecoderStatus(const QImage &frame, const std::shared_ptr<rtc::binary> &encodedReference, int width, int height);
    static PathProbeStatus buildPipelinePath(const QImage &frame, int width, int height, int fps,
                                             const ImplementationStatus &encoderStatus,
                                             const ImplementationStatus &decoderStatus,
                                             const std::shared_ptr<rtc::binary> &encodedReference,
                                             bool preferZeroCopy);
    static QJsonObject buildCodecEntry(const QString &codec, const QString &mode, PathStatus status, bool preferred = false);
    static QImage createProbeImage(int width, int height, int fps);
    static QImage createDesktopProbeImage(int width, int height);
    static bool looksReasonable(const QImage &input, const QImage &decoded, QString *reason = nullptr);
};

#endif