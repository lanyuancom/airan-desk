#include "ffmpeg_util.h"
#include "../common/logger_manager.h"

namespace
{
    bool codecNameContains(const QString &name, const char *needle)
    {
        return name.contains(needle, Qt::CaseInsensitive);
    }

    int platformHwTypeScore(AVHWDeviceType hwType, bool isEncoder)
    {
#if !AIRAN_FFMPEG_HAS_HWCONFIG
        Q_UNUSED(hwType);
        Q_UNUSED(isEncoder);
        return (0);
#else
#if defined(Q_OS_WIN)
        switch (hwType)
        {
        case AIRAN_AV_HWDEVICE_TYPE_D3D11VA:
            return isEncoder ? 3600 : 3000;
        case AIRAN_AV_HWDEVICE_TYPE_QSV:
            return isEncoder ? 3400 : 2600;
        case AIRAN_AV_HWDEVICE_TYPE_CUDA:
            return isEncoder ? 3000 : 2400;
        case AIRAN_AV_HWDEVICE_TYPE_DXVA2:
            return isEncoder ? 1400 : 2200;
        case AIRAN_AV_HWDEVICE_TYPE_D3D12VA:
            return isEncoder ? 1800 : 1800;
        case AIRAN_AV_HWDEVICE_TYPE_VULKAN:
            return isEncoder ? 900 : 1100;
        default:
            return 200;
        }
#elif defined(Q_OS_MACOS)
        switch (hwType)
        {
        case AIRAN_AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
            return isEncoder ? 3200 : 3400;
        default:
            return 150;
        }
#elif defined(Q_OS_LINUX)
        switch (hwType)
        {
        case AIRAN_AV_HWDEVICE_TYPE_VAAPI:
            return isEncoder ? 2800 : 2900;
        case AIRAN_AV_HWDEVICE_TYPE_VULKAN:
            return isEncoder ? 1800 : 2000;
        case AIRAN_AV_HWDEVICE_TYPE_DRM:
            return isEncoder ? 1500 : 1700;
        default:
            return 200;
        }
#else
        Q_UNUSED(isEncoder);
        switch (hwType)
        {
        case AIRAN_AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
            return 1200;
        case AIRAN_AV_HWDEVICE_TYPE_D3D11VA:
        case AIRAN_AV_HWDEVICE_TYPE_QSV:
        case AIRAN_AV_HWDEVICE_TYPE_CUDA:
            return 1000;
        case AIRAN_AV_HWDEVICE_TYPE_VAAPI:
            return 900;
        default:
            return 200;
        }
#endif
#endif
    }

    int platformCodecNameScore(const QString &codecName, bool isEncoder, bool isHardware)
    {
        int score = 0;

#if defined(Q_OS_WIN)
        if (isEncoder)
        {
            if (codecNameContains(codecName, "nvenc"))
                score += 14000;
            else if (codecNameContains(codecName, "qsv"))
                score += 13000;
            else if (codecNameContains(codecName, "amf"))
                score += 12000;
            else if (codecNameContains(codecName, "_mf"))
                score += 11000;
            else if (codecNameContains(codecName, "x264"))
                score += 9000;
            else if (codecNameContains(codecName, "openh264"))
                score += 7000;
        }
        else
        {
#if QT_POINTER_SIZE == 4
            /* 32 位 Windows 上老显卡/老驱动的 CUVID/QSV 解码器经常能打开但实际解码失败。 */
            /* 控制端优先保证可显示，硬解仍保留为软件解码不可用时的后备候选。 */
            if (!isHardware && codecName == "h264")
                score += 30000;
            if (codecNameContains(codecName, "cuvid") || codecNameContains(codecName, "cuda") || codecNameContains(codecName, "qsv"))
                score -= 20000;
#endif
            if (codecNameContains(codecName, "d3d11va"))
                score += 14000;
            else if (codecNameContains(codecName, "qsv"))
                score += 13000;
            else if (codecNameContains(codecName, "cuvid") || codecNameContains(codecName, "cuda"))
                score += 12000;
            else if (codecNameContains(codecName, "dxva2"))
                score += 11000;
            else if (!isHardware && codecName == "h264")
                score += 7000;
        }
#elif defined(Q_OS_MACOS)
        if (isEncoder)
        {
            if (codecNameContains(codecName, "videotoolbox"))
                score += 16000;
            else if (codecNameContains(codecName, "x264"))
                score += 11000;
            else if (codecNameContains(codecName, "openh264"))
                score += 8000;
        }
        else
        {
            if (codecNameContains(codecName, "videotoolbox"))
                score += 16000;
            else if (!isHardware && codecName == "h264")
                score += 9000;
        }
#elif defined(Q_OS_LINUX)
        if (isEncoder)
        {
            if (codecNameContains(codecName, "vaapi"))
                score += 14000;
            else if (codecNameContains(codecName, "v4l2m2m"))
                score += 12000;
            else if (codecNameContains(codecName, "x264"))
                score += 11000;
            else if (codecNameContains(codecName, "openh264"))
                score += 9000;
            else if (codecNameContains(codecName, "vulkan"))
                score += 7000;
        }
        else
        {
            if (codecNameContains(codecName, "vaapi"))
                score += 14000;
            else if (codecNameContains(codecName, "v4l2m2m"))
                score += 12000;
            else if (codecNameContains(codecName, "vulkan"))
                score += 10000;
            else if (!isHardware && codecName == "h264")
                score += 9000;
        }
#endif

        if (codecNameContains(codecName, "videotoolbox"))
            score += 3000;
        else if (codecNameContains(codecName, "nvenc") || codecNameContains(codecName, "cuvid") || codecNameContains(codecName, "nvdec"))
            score += 2800;
        else if (codecNameContains(codecName, "amf"))
            score += 2600;
        else if (codecNameContains(codecName, "qsv"))
            score += 2400;
        else if (codecNameContains(codecName, "vaapi"))
            score += 2200;
        else if (codecNameContains(codecName, "v4l2m2m"))
            score += 1800;
        else if (codecNameContains(codecName, "vulkan"))
            score += 1200;
        else if (codecNameContains(codecName, "x264"))
            score += 1000;
        else if (codecNameContains(codecName, "openh264"))
            score += 700;
        else if (!isHardware && codecName == "h264")
            score += 500;

        return score;
    }

    bool shouldAddSoftwareDecoderFallback(const CodecInfo &info)
    {
        if (info.type != CodecInfo::DECODER || !info.isHardware)
            return false;

        /* FFmpeg's native "h264" decoder can expose hardware configs while still */
        /* being a valid pure software decoder. Dedicated wrappers such as */
        /* h264_qsv/h264_cuvid/videotoolbox do not have that fallback behavior. */
        return info.name == QStringLiteral("h264");
    }

    bool hwAccelScoreGreater(const std::shared_ptr<HwAccelInfo> &a, const std::shared_ptr<HwAccelInfo> &b)
    {
        return a->score > b->score;
    }

    bool codecScoreGreater(const std::shared_ptr<CodecInfo> &a, const std::shared_ptr<CodecInfo> &b)
    {
        return a->score > b->score;
    }
} /* namespace */

QList<std::shared_ptr<CodecInfo>> FFmpegUtilData::getH264Encoders()
{
    init();
    return m_h264Encoders;
}
QList<std::shared_ptr<CodecInfo>> FFmpegUtilData::getH264Decoders()
{
    init();
    return m_h264Decoders;
}

void FFmpegUtilData::init()
{
    QMutexLocker locker(&mutex);
    if (m_inited)
    {
        return;
    }

#if LIBAVCODEC_VERSION_MAJOR < 58
    /* FFmpeg3.x需要显式注册编解码器，否则 av_codec_next()可能枚举不到 libx264 等外部编码器。 */
    /* FFmpeg5+ 已移除此 API，所以必须加版本保护，避免 Windows FFmpeg7.1 编译失败。 */
    avcodec_register_all();
#endif

    const AVCodec *codec = NULL;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 10, 100)
    void *iterator = NULL;
#endif

    /* 遍历所有编码器 */
    while ((codec =
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 10, 100)
                av_codec_iterate(&iterator)
#else
                av_codec_next(codec)
#endif
                ))
    {
        /* 筛选H.264编码器 */
        if (codec->id != AV_CODEC_ID_H264)
            continue;

        std::shared_ptr<CodecInfo> info = std::make_shared<CodecInfo>();
        info->name = codec->name ? codec->name : "";
        info->longName = codec->long_name ? codec->long_name : "";
        info->type = CodecInfo::UNKNOWN;
        if (av_codec_is_encoder(codec))
        {
            info->type = CodecInfo::ENCODER;
        }
        else if (av_codec_is_decoder(codec))
        {
            info->type = CodecInfo::DECODER;
        }
        info->isHardware = false;
        info->codec = codec;
        const bool isEncoder = (info->type == CodecInfo::ENCODER);
#if AIRAN_FFMPEG_HAS_HWCONFIG
        const AVCodecHWConfig *config = NULL;
        int hw_config_index = 0;
        while ((config = avcodec_get_hw_config(codec, hw_config_index++)))
        {
            info->isHardware = true;
            if (config->pix_fmt == AV_PIX_FMT_NONE)
            {
                /* 如果像素格式是 AV_PIX_FMT_NONE，表示该配置适用于所有像素格式，跳过以避免冗余 */
                continue;
            }
            auto hwAccelInfo = std::make_shared<HwAccelInfo>();
            hwAccelInfo->config = config;
            hwAccelInfo->hwDeviceType = config->device_type;
            hwAccelInfo->hwDeviceTypeName = av_hwdevice_get_type_name(config->device_type);

            hwAccelInfo->supportedPixFormats.append(config->pix_fmt);
            hwAccelInfo->supportedPixFormatNames.append(av_get_pix_fmt_name(config->pix_fmt));
            info->supportedHwTypes.append(hwAccelInfo);
            switch (hwAccelInfo->hwDeviceType)
            {
            case AIRAN_AV_HWDEVICE_TYPE_D3D11VA:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            case AIRAN_AV_HWDEVICE_TYPE_CUDA:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            case AIRAN_AV_HWDEVICE_TYPE_QSV:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            case AIRAN_AV_HWDEVICE_TYPE_VULKAN:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            case AIRAN_AV_HWDEVICE_TYPE_D3D12VA:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            case AIRAN_AV_HWDEVICE_TYPE_VAAPI:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            case AIRAN_AV_HWDEVICE_TYPE_DXVA2:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            case AIRAN_AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            case AIRAN_AV_HWDEVICE_TYPE_DRM:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            case AIRAN_AV_HWDEVICE_TYPE_OPENCL:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            case AIRAN_AV_HWDEVICE_TYPE_MEDIACODEC:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            case AIRAN_AV_HWDEVICE_TYPE_VDPAU:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            default:
                hwAccelInfo->score = platformHwTypeScore(hwAccelInfo->hwDeviceType, isEncoder);
                break;
            }
        }
#endif
        if (info->isHardware && info->supportedHwTypes.isEmpty())
        {
            /* 如果标记为支持硬件加速但没有有效的硬件配置，跳过该编码器 */
            continue;
        }
        if (info->isHardware)
        {
            info->score += 10000;
            info->score += info->name.size();
        }
        else
        {
            info->score = 100 - info->name.size();
        }
        info->score += platformCodecNameScore(info->name, isEncoder, info->isHardware);
        /* 对支持的硬件类型按分数排序 */
        std::sort(info->supportedHwTypes.begin(), info->supportedHwTypes.end(), hwAccelScoreGreater);
        if (info->type == CodecInfo::DECODER)
        {
            if (shouldAddSoftwareDecoderFallback(*info))
            {
                std::shared_ptr<CodecInfo> softwareInfo = std::make_shared<CodecInfo>();
                *softwareInfo = *info;
                softwareInfo->isHardware = false;
                softwareInfo->supportedHwTypes.clear();
                softwareInfo->score = 100 - softwareInfo->name.size();
                softwareInfo->score += platformCodecNameScore(softwareInfo->name, false, false);
                m_h264Decoders.push_back(softwareInfo);
                LOG_INFO("Added software fallback for H264 decoder: {}", softwareInfo->toString());
            }
            m_h264Decoders.push_back(info);
        }
        else if (info->type == CodecInfo::ENCODER)
        {
            if (info->name.contains("_mf"))
            {
                std::shared_ptr<CodecInfo> info2 = std::make_shared<CodecInfo>();
                *info2 = *info;
                info2->score += 1000;     /* Media Foundation 编码器，分数加1000 */
                info2->isHardware = true; /* 强制标记为硬件编码器，优先使用 */
                m_h264Encoders.push_back(info2);
            }
            m_h264Encoders.push_back(info);
        }
    }

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
    /* FFmpeg3.x 的 av_codec_next() 在部分发行版/链接组合下可能拿不到 */
    /* 外部注册编码器列表；显式按名称补齐常见 H264 编码器，至少保证 */
    /* Ubuntu18.04 的 libx264 可作为稳定的软件编码回退。 */
    const char *fallbackEncoderNames[] = {
        "libx264",
        "libx264rgb",
        "h264_vaapi",
        "h264_nvenc",
        "h264_v4l2m2m",
        "h264_omx",
        "nvenc_h264",
        "nvenc",
        nullptr};
    for (const char **name = fallbackEncoderNames; *name; ++name)
    {
        const AVCodec *fallbackCodec = avcodec_find_encoder_by_name(*name);
        if (!fallbackCodec || fallbackCodec->id != AV_CODEC_ID_H264)
            continue;
        bool exists = false;
        for (const auto &existing : m_h264Encoders)
        {
            if (existing && existing->codec == fallbackCodec)
            {
                exists = true;
                break;
            }
        }
        if (exists)
            continue;

        std::shared_ptr<CodecInfo> info = std::make_shared<CodecInfo>();
        info->name = fallbackCodec->name ? fallbackCodec->name : "";
        info->longName = fallbackCodec->long_name ? fallbackCodec->long_name : "";
        info->type = CodecInfo::ENCODER;
        info->codec = fallbackCodec;
        info->isHardware = info->name.contains("vaapi", Qt::CaseInsensitive) ||
                           info->name.contains("nvenc", Qt::CaseInsensitive) ||
                           info->name.contains("v4l2m2m", Qt::CaseInsensitive) ||
                           info->name.contains("omx", Qt::CaseInsensitive);
        if (info->isHardware)
        {
            info->score = 10000 + info->name.size();
        }
        else
        {
            info->score = 100 - info->name.size();
        }
        info->score += platformCodecNameScore(info->name, true, info->isHardware);

        if (info->isHardware && info->supportedHwTypes.isEmpty())
        {
#if AIRAN_FFMPEG_HAS_HWCONFIG
            AVHWDeviceType hwType = AIRAN_AV_HWDEVICE_TYPE_NONE;
            if (info->name.contains("vaapi", Qt::CaseInsensitive))
                hwType = AIRAN_AV_HWDEVICE_TYPE_VAAPI;
            else if (info->name.contains("nvenc", Qt::CaseInsensitive))
                hwType = AIRAN_AV_HWDEVICE_TYPE_CUDA;
            else if (info->name.contains("v4l2m2m", Qt::CaseInsensitive))
                hwType = AIRAN_AV_HWDEVICE_TYPE_DRM;

            if (hwType != AIRAN_AV_HWDEVICE_TYPE_NONE)
            {
                auto hwAccelInfo = std::make_shared<HwAccelInfo>();
                hwAccelInfo->hwDeviceType = hwType;
                const char *typeName = av_hwdevice_get_type_name(hwType);
                hwAccelInfo->hwDeviceTypeName = typeName ? typeName : "";
                hwAccelInfo->supportedPixFormats.append(AV_PIX_FMT_NV12);
                hwAccelInfo->supportedPixFormatNames.append(av_get_pix_fmt_name(AV_PIX_FMT_NV12));
                hwAccelInfo->score = platformHwTypeScore(hwType, true);
                info->supportedHwTypes.append(hwAccelInfo);
            }
#endif
        }

        if (info->isHardware && info->supportedHwTypes.isEmpty())
        {
            /* 没有可用 hwdevice 映射的旧硬件封装器不要加入，否则只会初始化失败。 */
            LOG_WARN("Skipping legacy H264 hardware encoder without usable hwdevice mapping: {}", info->name);
            continue;
        }
        LOG_INFO("Added fallback H264 encoder by name for FFmpeg3.x: {}", info->toString());
        m_h264Encoders.push_back(info);
    }
#endif

    if (m_h264Encoders.isEmpty())
    {
        const AVCodec *libx264 = avcodec_find_encoder_by_name("libx264");
        if (libx264)
        {
            std::shared_ptr<CodecInfo> info = std::make_shared<CodecInfo>();
            info->name = libx264->name ? libx264->name : "libx264";
            info->longName = libx264->long_name ? libx264->long_name : "";
            info->type = CodecInfo::ENCODER;
            info->codec = libx264;
            info->isHardware = false;
            info->score = 10000;
            LOG_WARN("H264 encoder enumeration returned empty, forcing libx264 fallback: {}", info->toString());
            m_h264Encoders.push_back(info);
        }
    }
    /* 硬编码优先 */
    std::sort(m_h264Encoders.begin(), m_h264Encoders.end(), codecScoreGreater);

    LOG_DEBUG("FFMPEG h264 encoders :{}", m_h264Encoders.size());
    for (const std::shared_ptr<CodecInfo> &info : m_h264Encoders)
    {
        LOG_DEBUG("{} ", info->toString());
    }
    /* 硬解码优先 */
    std::sort(m_h264Decoders.begin(), m_h264Decoders.end(), codecScoreGreater);
    LOG_DEBUG("FFMPEG h264 decoders :{}", m_h264Decoders.size());
    for (const std::shared_ptr<CodecInfo> &info : m_h264Decoders)
    {
        LOG_DEBUG("{} ", info->toString());
    }

    m_inited = true;
}

FFmpegUtilData::~FFmpegUtilData()
{
    cleanup();
}

void FFmpegUtilData::cleanup()
{
    QMutexLocker locker(&mutex);
    m_h264Encoders.clear();
    m_h264Decoders.clear();
}

FFmpegUtilData *FFmpegUtilData::instance()
{
    static FFmpegUtilData ffmpegUtilData;
    ffmpegUtilData.init();
    return &ffmpegUtilData;
}

FFmpegUtilData::FFmpegUtilData()
{
    avdevice_register_all();
}
