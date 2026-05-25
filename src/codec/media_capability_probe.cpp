#include "media_capability_probe.h"

#include "../common/constant.h"
#include "h264_decoder.h"
#include "h264_encoder.h"

#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QtMath>

QJsonObject MediaCapabilityProbe::ImplementationStatus::toJson() const
{
    return QJsonObject{{Constant::KEY_NAME, name},
                       {Constant::KEY_IMPLEMENTATION, implementation},
                       {Constant::KEY_IMPL_TYPE, implType},
                       {Constant::KEY_IS_HARDWARE, isHardware},
                       {Constant::KEY_OPENABLE, openable},
                       {Constant::KEY_PRIORITY, priority},
                       {Constant::KEY_STATUS, MediaCapabilityProbe::pathStatusToString(status)},
                       {Constant::KEY_FRAMES_TESTED, framesTested},
                       {Constant::KEY_STABLE_FRAMES, stableFrames},
                       {"warnings", warnings}};
}

QJsonObject MediaCapabilityProbe::PathProbeStatus::toJson() const
{
    return QJsonObject{{Constant::KEY_NAME, name},
                       {Constant::KEY_ENCODE, encodeImpl},
                       {Constant::KEY_DECODE, decodeImpl},
                       {Constant::KEY_ZERO_COPY, zeroCopy},
                       {Constant::KEY_STATUS, MediaCapabilityProbe::pathStatusToString(status)},
                       {Constant::KEY_FRAMES_TESTED, framesTested},
                       {Constant::KEY_STABLE_FRAMES, stableFrames},
                       {"warnings", warnings}};
}

QJsonObject MediaCapabilityProbe::ProbeResult::toJson() const
{
    return QJsonObject{{Constant::KEY_CODEC, codec},
                       {"encoder_available", encoderAvailable},
                       {"decoder_available", decoderAvailable},
                       {"loopback_stable", loopbackStable},
                       {"encoder_name", encoderName},
                       {"decoder_name", decoderName},
                       {Constant::KEY_PROFILE, recommendedProfile},
                       {Constant::KEY_PACKETIZATION_MODE, packetizationMode},
                       {Constant::KEY_PROFILE_LEVEL_ID, profileLevelId},
                       {Constant::KEY_WIDTH, width},
                       {Constant::KEY_HEIGHT, height},
                       {Constant::KEY_FPS, fps},
                       {Constant::KEY_BITRATE, bitrate},
                       {Constant::KEY_ZERO_COPY, zeroCopy},
                       {Constant::KEY_ALLOW_HIGH_PERFORMANCE, allowHighPerformance},
                       {Constant::KEY_CODECS, codecs},
                       {Constant::KEY_SUPPORTED_CODECS, supportedCodecs},
                       {Constant::KEY_SUPPORTED_PROFILES, supportedProfiles},
                       {Constant::KEY_IMPLEMENTATIONS, implementations},
                       {Constant::KEY_ENCODE, encode},
                       {Constant::KEY_DECODE, decode},
                       {Constant::KEY_PATHS, paths},
                       {Constant::KEY_MAX_STABLE, maxStable},
                       {Constant::KEY_RECOMMENDED, recommended},
                       {Constant::KEY_RECOMMENDED_SEND, recommendedSend},
                       {Constant::KEY_RECOMMENDED_RECEIVE, recommendedReceive},
                       {"warnings", warnings}};
}

MediaCapabilityProbe::ProbeResult MediaCapabilityProbe::run(int width, int height, int fps)
{
    ProbeResult result;
    result.width = width;
    result.height = height;
    result.fps = fps;
    const double pixels = static_cast<double>(qMax(1, width)) * static_cast<double>(qMax(1, height));
    const double fpsScale = qBound(0.5, static_cast<double>(qMax(1, fps)) / 30.0, 4.0);
    /* 桌面/文字场景优先保真：网络好时给足码率，弱网由运行时控制下调，避免一开始就因码率过低产生马赛克。 */
    result.bitrate = qBound(2500000,
                            static_cast<int>((pixels / (1280.0 * 720.0)) * 5000000.0 * fpsScale),
                            50000000);
    result.supportedCodecs = QJsonArray{QStringLiteral("H264")};
    result.supportedProfiles = QJsonArray{QStringLiteral("baseline"), QStringLiteral("main")};

    const QImage syntheticProbe = createProbeImage(width, height, fps);
    const QImage desktopProbe = createDesktopProbeImage(width, height);
    const QImage probe = desktopProbe.isNull() ? syntheticProbe : desktopProbe;

    const ImplementationStatus encoderStatus = buildEncoderStatus(width, height, fps, probe);
    result.implementations.append(encoderStatus.toJson());
    result.encoderAvailable = encoderStatus.status != PathStatus::Disabled;
    result.encoderName = encoderStatus.implementation;
    result.encode = QJsonObject{{Constant::KEY_IMPLEMENTATION, encoderStatus.implementation},
                                {Constant::KEY_IMPL_TYPE, encoderStatus.implType},
                                {Constant::KEY_STATUS, pathStatusToString(encoderStatus.status)},
                                {Constant::KEY_OPENABLE, encoderStatus.openable},
                                {Constant::KEY_FRAMES_TESTED, encoderStatus.framesTested},
                                {Constant::KEY_STABLE_FRAMES, encoderStatus.stableFrames},
                                {Constant::KEY_CODEC, QStringLiteral("H264")}};

    if (!result.encoderAvailable)
    {
        result.warnings.append(QStringLiteral("encoder_init_failed"));
        result.maxStable = QJsonObject{{Constant::KEY_WIDTH, width},
                                       {Constant::KEY_HEIGHT, height},
                                       {Constant::KEY_FPS, fps},
                                       {Constant::KEY_BITRATE, result.bitrate}};
        result.recommendedSend = buildRecommended(result, Constant::KEY_SEND_HINT);
        result.recommendedReceive = buildRecommended(result, Constant::KEY_RECEIVE_HINT);
        result.recommended = result.recommendedSend;
        return result;
    }

    H264Encoder encoder;
    if (!encoder.initialize(0, width, height, fps))
    {
        result.warnings.append(QStringLiteral("encoder_init_failed"));
        result.maxStable = QJsonObject{{Constant::KEY_WIDTH, width},
                                       {Constant::KEY_HEIGHT, height},
                                       {Constant::KEY_FPS, fps},
                                       {Constant::KEY_BITRATE, result.bitrate}};
        result.recommendedSend = buildRecommended(result, Constant::KEY_SEND_HINT);
        result.recommendedReceive = buildRecommended(result, Constant::KEY_RECEIVE_HINT);
        result.recommended = result.recommendedSend;
        return result;
    }

    auto encoded = encoder.encodeCPU(probe);
    if (!encoded.first || encoded.first->empty())
    {
        result.warnings.append(QStringLiteral("encode_empty"));
        result.maxStable = QJsonObject{{Constant::KEY_WIDTH, width},
                                       {Constant::KEY_HEIGHT, height},
                                       {Constant::KEY_FPS, fps},
                                       {Constant::KEY_BITRATE, result.bitrate}};
        result.recommendedSend = buildRecommended(result, Constant::KEY_SEND_HINT);
        result.recommendedReceive = buildRecommended(result, Constant::KEY_RECEIVE_HINT);
        result.recommended = result.recommendedSend;
        return result;
    }

    const ImplementationStatus decoderStatus = buildDecoderStatus(probe, encoded.first, width, height);
    result.implementations.append(decoderStatus.toJson());
    result.decoderAvailable = decoderStatus.status != PathStatus::Disabled;
    result.decoderName = decoderStatus.implementation;
    result.decode = QJsonObject{{Constant::KEY_IMPLEMENTATION, decoderStatus.implementation},
                                {Constant::KEY_IMPL_TYPE, decoderStatus.implType},
                                {Constant::KEY_STATUS, pathStatusToString(decoderStatus.status)},
                                {Constant::KEY_OPENABLE, decoderStatus.openable},
                                {Constant::KEY_FRAMES_TESTED, decoderStatus.framesTested},
                                {Constant::KEY_STABLE_FRAMES, decoderStatus.stableFrames},
                                {Constant::KEY_CODEC, QStringLiteral("H264")}};

    if (!result.decoderAvailable)
    {
        result.warnings.append(QStringLiteral("decoder_init_failed"));
        result.maxStable = QJsonObject{{Constant::KEY_WIDTH, width},
                                       {Constant::KEY_HEIGHT, height},
                                       {Constant::KEY_FPS, fps},
                                       {Constant::KEY_BITRATE, result.bitrate}};
        result.recommendedSend = buildRecommended(result, Constant::KEY_SEND_HINT);
        result.recommendedReceive = buildRecommended(result, Constant::KEY_RECEIVE_HINT);
        result.recommended = result.recommendedSend;
        return result;
    }

    H264Decoder decoder;
    if (!decoder.initialize())
    {
        result.warnings.append(QStringLiteral("decoder_init_failed"));
        result.maxStable = QJsonObject{{Constant::KEY_WIDTH, width},
                                       {Constant::KEY_HEIGHT, height},
                                       {Constant::KEY_FPS, fps},
                                       {Constant::KEY_BITRATE, result.bitrate}};
        result.recommendedSend = buildRecommended(result, Constant::KEY_SEND_HINT);
        result.recommendedReceive = buildRecommended(result, Constant::KEY_RECEIVE_HINT);
        result.recommended = result.recommendedSend;
        return result;
    }

    QString reason;
    const QImage decoded = decoder.decodeFrame(*encoded.first);
    result.loopbackStable = looksReasonable(probe, decoded, &reason);
    if (!result.loopbackStable)
    {
        result.warnings.append(reason.isEmpty() ? QStringLiteral("decoded_unreasonable") : reason);
    }

    result.zeroCopy = false;
    result.codec = QStringLiteral("h264");
    result.recommendedProfile = QStringLiteral("baseline");
    const PathProbeStatus cpuToCpu = buildPipelinePath(probe, width, height, fps, encoderStatus, decoderStatus, encoded.first, false);
    result.paths.append(cpuToCpu.toJson());
    if (encoderStatus.isHardware)
    {
        PathProbeStatus cpuToHwSw = cpuToCpu;
        cpuToHwSw.name = QStringLiteral("cpu_capture_hardware_encode_software_decode");
        cpuToHwSw.encodeImpl = encoderStatus.implementation;
        cpuToHwSw.decodeImpl = QStringLiteral("software_fallback");
        cpuToHwSw.status = encoderStatus.status == PathStatus::Stable ? PathStatus::Stable : PathStatus::Experimental;
        result.paths.append(cpuToHwSw.toJson());

        PathProbeStatus cpuToHwHw = cpuToCpu;
        cpuToHwHw.name = QStringLiteral("cpu_capture_hardware_encode_hardware_decode");
        cpuToHwHw.encodeImpl = encoderStatus.implementation;
        cpuToHwHw.decodeImpl = decoderStatus.implementation;
        cpuToHwHw.status = (encoderStatus.status == PathStatus::Stable && decoderStatus.status == PathStatus::Stable)
                               ? PathStatus::Stable
                               : PathStatus::Experimental;
        result.paths.append(cpuToHwHw.toJson());
    }
    result.maxStable = QJsonObject{{Constant::KEY_WIDTH, width},
                                   {Constant::KEY_HEIGHT, height},
                                   {Constant::KEY_FPS, fps},
                                   {Constant::KEY_BITRATE, result.bitrate}};
    result.codecs = QJsonArray{
        buildCodecEntry(QStringLiteral("H264"),
                        encoderStatus.isHardware ? QStringLiteral("hardware") : QStringLiteral("software"),
                        encoderStatus.status,
                        true)};
    result.recommendedSend = buildRecommended(result, Constant::KEY_SEND_HINT);
    result.recommendedReceive = buildRecommended(result, Constant::KEY_RECEIVE_HINT);
    result.recommended = result.recommendedSend;
    result.recommended.insert(Constant::KEY_CODEC_MODE,
                              encoderStatus.isHardware ? QStringLiteral("hardware") : QStringLiteral("software"));
    result.recommended.insert(Constant::KEY_SELECTED_CODEC, QStringLiteral("H264"));
    result.recommendedSend.insert(Constant::KEY_CODEC_MODE,
                                  encoderStatus.isHardware ? QStringLiteral("hardware") : QStringLiteral("software"));
    result.recommendedSend.insert(Constant::KEY_SELECTED_CODEC, QStringLiteral("H264"));
    result.recommendedReceive.insert(Constant::KEY_CODEC_MODE,
                                     decoderStatus.isHardware ? QStringLiteral("hardware") : QStringLiteral("software"));
    result.recommendedReceive.insert(Constant::KEY_SELECTED_CODEC, QStringLiteral("H264"));
    result.recommendedReceive.insert(Constant::KEY_IMPLEMENTATION, decoderStatus.implementation);
    result.recommendedSend.insert(Constant::KEY_IMPLEMENTATION, encoderStatus.implementation);
    result.recommended.insert(Constant::KEY_IMPLEMENTATION, encoderStatus.implementation);
    return result;
}

QString MediaCapabilityProbe::pathStatusToString(PathStatus status)
{
    switch (status)
    {
    case PathStatus::Available:
        return Constant::KEY_AVAILABLE;
    case PathStatus::Stable:
        return Constant::KEY_STABLE;
    case PathStatus::Experimental:
        return Constant::KEY_EXPERIMENTAL;
    case PathStatus::Disabled:
    default:
        return Constant::KEY_DISABLED;
    }
}

QJsonObject MediaCapabilityProbe::buildRecommended(const ProbeResult &result, const QString &directionHint)
{
    return QJsonObject{{Constant::KEY_CODEC, result.codec.toUpper()},
                       {Constant::KEY_PROFILE, result.recommendedProfile},
                       {Constant::KEY_PACKETIZATION_MODE, result.packetizationMode},
                       {Constant::KEY_PROFILE_LEVEL_ID, result.profileLevelId},
                       {directionHint == Constant::KEY_RECEIVE_HINT ? Constant::KEY_RECEIVE_HINT : Constant::KEY_SEND_HINT, true},
                       {Constant::KEY_WIDTH, result.width},
                       {Constant::KEY_HEIGHT, result.height},
                       {Constant::KEY_FPS, result.fps},
                       {Constant::KEY_BITRATE, result.bitrate}};
}

MediaCapabilityProbe::ImplementationStatus MediaCapabilityProbe::buildEncoderStatus(int width, int height, int fps, const QImage &frame)
{
    ImplementationStatus status;
    status.name = QStringLiteral("encoder");
    status.implType = QStringLiteral("encoder");
    status.priority = 100;

    H264Encoder encoder;
    if (!encoder.initialize(0, width, height, fps))
    {
        status.implementation = QStringLiteral("unknown");
        status.openable = false;
        status.status = PathStatus::Disabled;
        status.warnings.append(QStringLiteral("encoder_init_failed"));
        return status;
    }

    status.implementation = encoder.implementationName();
    status.isHardware = encoder.isHardwareEncoder();
    status.openable = true;
    status.framesTested = 1;

    if (status.implementation.compare(QStringLiteral("h264_mf"), Qt::CaseInsensitive) == 0)
    {
        status.status = PathStatus::Experimental;
        status.stableFrames = 0;
        status.warnings.append(QStringLiteral("encoder_backend_known_unstable"));
        return status;
    }

    auto encoded = encoder.encodeCPU(frame);
    if (!encoded.first || encoded.first->empty())
    {
        status.status = PathStatus::Available;
        status.stableFrames = 0;
        status.warnings.append(QStringLiteral("encode_empty"));
        return status;
    }

    status.status = PathStatus::Stable;
    status.stableFrames = 1;
    return status;
}

MediaCapabilityProbe::ImplementationStatus MediaCapabilityProbe::buildDecoderStatus(const QImage &frame, const std::shared_ptr<rtc::binary> &encodedReference, int width, int height)
{
    Q_UNUSED(width)
    Q_UNUSED(height)

    ImplementationStatus status;
    status.name = QStringLiteral("decoder");
    status.implType = QStringLiteral("decoder");
    status.priority = 100;

    H264Decoder decoder;
    if (!decoder.initialize())
    {
        status.implementation = QStringLiteral("unknown");
        status.openable = false;
        status.status = PathStatus::Disabled;
        status.warnings.append(QStringLiteral("decoder_init_failed"));
        return status;
    }

    status.implementation = decoder.implementationName();
    status.openable = true;
    status.framesTested = encodedReference && !encodedReference->empty() ? 1 : 0;
    status.isHardware = !status.implementation.isEmpty() &&
                        !status.implementation.contains(QStringLiteral("264"), Qt::CaseInsensitive);

    const QString implLower = status.implementation.toLower();
    if (status.isHardware && (implLower.contains(QStringLiteral("d3d11")) ||
                              implLower.contains(QStringLiteral("dxva")) ||
                              implLower.contains(QStringLiteral("qsv")) ||
                              implLower.contains(QStringLiteral("cuda")) ||
                              implLower.contains(QStringLiteral("nvdec"))))
    {
        status.status = PathStatus::Experimental;
        status.warnings.append(QStringLiteral("hardware_decoder_requires_runtime_fallback"));
    }

    if (!encodedReference || encodedReference->empty())
    {
        if (status.status == PathStatus::Disabled)
            status.status = PathStatus::Available;
        status.warnings.append(QStringLiteral("missing_encoded_reference"));
        return status;
    }

    QString reason;
    const QImage decoded = decoder.decodeFrame(*encodedReference);
    const bool reasonable = looksReasonable(frame, decoded, &reason);
    if (reasonable)
    {
        if (status.status != PathStatus::Experimental)
            status.status = PathStatus::Stable;
        status.stableFrames = status.framesTested;
    }
    else
    {
        status.status = PathStatus::Experimental;
        status.stableFrames = 0;
    }
    if (!reason.isEmpty() && status.status != PathStatus::Stable)
    {
        status.warnings.append(reason);
    }
    return status;
}

MediaCapabilityProbe::PathProbeStatus MediaCapabilityProbe::buildPipelinePath(const QImage &frame,
                                                                              int width,
                                                                              int height,
                                                                              int fps,
                                                                              const ImplementationStatus &encoderStatus,
                                                                              const ImplementationStatus &decoderStatus,
                                                                              const std::shared_ptr<rtc::binary> &encodedReference,
                                                                              bool preferZeroCopy)
{
    Q_UNUSED(width)
    Q_UNUSED(height)
    Q_UNUSED(fps)

    PathProbeStatus path;
    path.name = QStringLiteral("cpu_capture_software_encode_software_decode");
    path.encodeImpl = encoderStatus.implementation;
    path.decodeImpl = decoderStatus.implementation;
    path.zeroCopy = preferZeroCopy;
    path.framesTested = encodedReference && !encodedReference->empty() ? 1 : 0;
    path.stableFrames = (encoderStatus.status == PathStatus::Stable && decoderStatus.status == PathStatus::Stable) ? path.framesTested : 0;
    path.status = path.stableFrames > 0 ? PathStatus::Stable : PathStatus::Experimental;
    if (frame.isNull())
    {
        path.status = PathStatus::Disabled;
        path.warnings.append(QStringLiteral("missing_probe_frame"));
    }
    if (preferZeroCopy)
    {
        path.status = PathStatus::Experimental;
        path.warnings.append(QStringLiteral("zero_copy_not_verified_in_probe"));
    }
    return path;
}

QImage MediaCapabilityProbe::createProbeImage(int width, int height, int fps)
{
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::black);

    QPainter painter(&image);
    QLinearGradient grad(0, 0, width, height);
    grad.setColorAt(0.0, QColor(255, 0, 0));
    grad.setColorAt(0.5, QColor(0, 255, 0));
    grad.setColorAt(1.0, QColor(0, 0, 255));
    painter.fillRect(image.rect(), grad);
    painter.setPen(Qt::white);
    painter.drawRect(20, 20, width / 3, height / 3);
    painter.drawText(40, 60, QString("probe %1x%2 @%3fps").arg(width).arg(height).arg(fps));
    painter.drawText(40, 100, QStringLiteral("airan desk capability self-test"));
    return image;
}

QJsonObject MediaCapabilityProbe::buildCodecEntry(const QString &codec, const QString &mode, PathStatus status, bool preferred)
{
    return QJsonObject{{Constant::KEY_CODEC, codec},
                       {Constant::KEY_CODEC_MODE, mode},
                       {Constant::KEY_STATUS, pathStatusToString(status)},
                       {Constant::KEY_PREFERRED, preferred}};
}

QImage MediaCapabilityProbe::createDesktopProbeImage(int width, int height)
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
        return QImage();

    const QPixmap pixmap = screen->grabWindow(0);
    if (pixmap.isNull())
        return QImage();

    return pixmap.toImage().convertToFormat(QImage::Format_ARGB32).scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

bool MediaCapabilityProbe::looksReasonable(const QImage &input, const QImage &decoded, QString *reason)
{
    if (decoded.isNull())
    {
        if (reason)
            *reason = "decoded_empty";
        return false;
    }
    if (decoded.size() != input.size())
    {
        if (reason)
            *reason = "decoded_size_mismatch";
        return false;
    }

    const QPoint samples[] = {QPoint(input.width() / 4, input.height() / 4),
                              QPoint(input.width() / 2, input.height() / 2),
                              QPoint(input.width() * 3 / 4, input.height() * 3 / 4)};
    int diffBudget = 0;
    for (const QPoint &pt : samples)
    {
        const QColor a = input.pixelColor(pt);
        const QColor b = decoded.pixelColor(pt);
        diffBudget += qAbs(a.red() - b.red());
        diffBudget += qAbs(a.green() - b.green());
        diffBudget += qAbs(a.blue() - b.blue());
    }

    if (diffBudget > 420)
    {
        if (reason)
            *reason = "decoded_color_deviation_too_large";
        return false;
    }
    return true;
}