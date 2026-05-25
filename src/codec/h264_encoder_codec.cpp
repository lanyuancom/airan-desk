#include "h264_encoder.h"
#include "hardware_context_manager.h"
#include "h264_profile_level.h"

#include <QThread>
#include <algorithm>

bool H264Encoder::initializeCodec(std::shared_ptr<CodecInfo> codecInfo)
{
    const bool isMediaFoundation = codecInfo->name.contains("_mf", Qt::CaseInsensitive) ||
                                   codecInfo->longName.contains("MediaFoundation", Qt::CaseInsensitive);
    const bool isOpenH264 = codecInfo->name.contains("openh264", Qt::CaseInsensitive) ||
                            codecInfo->longName.contains("OpenH264", Qt::CaseInsensitive);
    const bool isX264 = codecInfo->name.contains("x264", Qt::CaseInsensitive) ||
                        codecInfo->longName.contains("x264", Qt::CaseInsensitive);
    const bool isVideoToolbox = codecInfo->name.contains("videotoolbox", Qt::CaseInsensitive) ||
                                codecInfo->longName.contains("VideoToolbox", Qt::CaseInsensitive);
    const bool isVaapi = codecInfo->name.contains("vaapi", Qt::CaseInsensitive);
    const bool isQsv = codecInfo->name.contains("qsv", Qt::CaseInsensitive);
    const bool isNvenc = codecInfo->name.contains("nvenc", Qt::CaseInsensitive);
    const bool isAmf = codecInfo->name.contains("amf", Qt::CaseInsensitive);

    /* 已验证 h264_mf 软编码在当前链路下容易出现局部花屏，优先跳过并回退到其他编码器。 */
    if (isMediaFoundation && !codecInfo->isHardware)
    {
        LOG_WARN("Skipping software MediaFoundation encoder {} due to artifact risk, trying next", codecInfo->name);
        return false;
    }

    /* 创建编码器上下文 */
    m_codecContext = avcodec_alloc_context3(codecInfo->codec);
    if (!m_codecContext)
    {
        LOG_ERROR("Could not allocate video codec context");
        return false;
    }

    /* 设置编码参数 */
    m_codecContext->width = m_dstW;
    m_codecContext->height = m_dstH;
    m_codecContext->bit_rate = m_bitrate;
    m_codecContext->rc_max_rate = m_bitrate;
    m_codecContext->rc_min_rate = std::max<int64_t>(100000, m_bitrate * 8 / 10);
    m_codecContext->rc_buffer_size = std::max<int>(m_bitrate / 2, 500000);
    m_codecContext->time_base = AVRational{1, m_fps};
    m_codecContext->framerate = AVRational{m_fps, 1};
    m_codecContext->gop_size = m_fps; /* 默认 1 秒 1 个关键帧，降低错误传播时长 */
    m_codecContext->max_b_frames = 0; /* 不使用B帧，只使用I帧和P帧 */
    m_codecContext->keyint_min = std::max(1, m_fps / 2);
    m_codecContext->has_b_frames = 0;
    m_codecContext->refs = 1;
#ifdef FF_PROFILE_H264_CONSTRAINED_BASELINE
    m_codecContext->profile = FF_PROFILE_H264_CONSTRAINED_BASELINE;
#else
    m_codecContext->profile = FF_PROFILE_H264_BASELINE;
#endif
    m_codecContext->level = H264ProfileLevel::levelIdcFor(m_dstW, m_dstH, m_fps);
    /* 选择编码器像素格式：优先 NV12，其次 YUV420P/YUVJ420P，再退回 NV12 */
    m_codecContext->pix_fmt = AV_PIX_FMT_NV12; /* 默认 */
    const enum AVPixelFormat *codecPixFmts = nullptr;
#if AIRAN_FFMPEG_HAS_SUPPORTED_CONFIG
    int pixFmtCount = 0;
    if (codecInfo->codec &&
        avcodec_get_supported_config(nullptr, codecInfo->codec, AV_CODEC_CONFIG_PIX_FORMAT,
                                     0, reinterpret_cast<const void **>(&codecPixFmts), &pixFmtCount) < 0)
    {
        codecPixFmts = nullptr;
    }
#else
    if (codecInfo->codec)
        codecPixFmts = codecInfo->codec->pix_fmts;
#endif
    if (codecPixFmts)
    {
        AVPixelFormat preferred[] = {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_BGRA, AV_PIX_FMT_BGR0};
        if (m_forceAllKeyframes && !codecInfo->isHardware)
        {
            preferred[0] = AV_PIX_FMT_YUV420P;
            preferred[1] = AV_PIX_FMT_NV12;
            preferred[2] = AV_PIX_FMT_YUVJ420P;
            preferred[3] = AV_PIX_FMT_BGRA;
            preferred[4] = AV_PIX_FMT_BGR0;
        }
#if defined(Q_OS_WIN)
        if (isNvenc || isQsv || isAmf || isMediaFoundation)
        {
            preferred[0] = AV_PIX_FMT_NV12;
            preferred[1] = AV_PIX_FMT_BGRA;
            preferred[2] = AV_PIX_FMT_BGR0;
            preferred[3] = AV_PIX_FMT_YUV420P;
            preferred[4] = AV_PIX_FMT_YUVJ420P;
        }
#elif defined(Q_OS_MACOS)
        if (isVideoToolbox)
        {
            preferred[0] = AV_PIX_FMT_NV12;
            preferred[1] = AV_PIX_FMT_YUV420P;
            preferred[2] = AV_PIX_FMT_BGRA;
            preferred[3] = AV_PIX_FMT_BGR0;
            preferred[4] = AV_PIX_FMT_YUVJ420P;
        }
#elif defined(Q_OS_LINUX)
        if (isVaapi)
        {
            preferred[0] = AV_PIX_FMT_NV12;
            preferred[1] = AV_PIX_FMT_YUV420P;
            preferred[2] = AV_PIX_FMT_BGRA;
            preferred[3] = AV_PIX_FMT_BGR0;
            preferred[4] = AV_PIX_FMT_YUVJ420P;
        }
#endif
        AVPixelFormat chosen = AV_PIX_FMT_NONE;
        const enum AVPixelFormat *p = codecPixFmts;
        for (; *p != AV_PIX_FMT_NONE; ++p)
        {
            for (AVPixelFormat pref : preferred)
            {
                if (*p == pref)
                {
                    chosen = *p;
                    break;
                }
            }
            if (chosen != AV_PIX_FMT_NONE)
                break;
        }
        if (chosen != AV_PIX_FMT_NONE)
            m_codecContext->pix_fmt = chosen;
        LOG_DEBUG("Codec {} supports pix_fmts, chosen pix_fmt={} (preferred)", codecInfo->name, av_get_pix_fmt_name(m_codecContext->pix_fmt));
    }

    /* 网络自适应优化：针对高延迟网络的编码参数 */
    /* 禁用全局头部（强制输出 Annex-B） */
    m_codecContext->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER;
    m_codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecContext->flags2 |= AV_CODEC_FLAG2_FAST;
    /* 多 slice 在弱网下更易出现局部马赛克，默认使用单 slice 以提高稳定性 */
    m_codecContext->slices = 1;
    /* 避免将 x264 风格参数下发给 AMF/Vulkan 等不兼容编码器 */
    if (isX264 || isOpenH264)
    {
        av_opt_set(m_codecContext->priv_data, "profile", "baseline", AV_OPT_SEARCH_CHILDREN); /* 使用 baseline profile 提高兼容性 */
        av_opt_set(m_codecContext->priv_data, "tune", "zerolatency", AV_OPT_SEARCH_CHILDREN);
    }
    av_opt_set(m_codecContext->priv_data, "profile", "baseline", AV_OPT_SEARCH_CHILDREN);
    av_opt_set(m_codecContext->priv_data, "level",
               QString::number(m_codecContext->level / 10.0, 'f', 1).toUtf8().constData(),
               AV_OPT_SEARCH_CHILDREN);
    av_opt_set(m_codecContext->priv_data, "annexb", "1", AV_OPT_SEARCH_CHILDREN);
    /* --- 硬件类 (NVENC, QSV, AMF) --- */
    /* 绝大多数硬件编码器使用 repeat_headers */
    av_opt_set_int(m_codecContext->priv_data, "repeat_headers", 1, AV_OPT_SEARCH_CHILDREN);
    /* 针对 MediaFoundation (h264_mf) 的调优：常见于 Windows 屏幕远程场景 */
    if (isMediaFoundation)
    {
        LOG_INFO("Applying MediaFoundation-specific encoder options for {}", codecInfo->toString());
        /* 使用 CBR 或低延迟 VBR，优先保证画面稳定 */
        av_opt_set_int(m_codecContext->priv_data, "rate_control", 0, AV_OPT_SEARCH_CHILDREN); /* 0 = cbr */
        /* 场景为 display remoting，提高远程桌面质量/延迟优化 */
        av_opt_set_int(m_codecContext->priv_data, "scenario", 1, AV_OPT_SEARCH_CHILDREN); /* 1 = display_remoting */
        /* 提高质量参数（0-100），-1 为默认 */
        av_opt_set_int(m_codecContext->priv_data, "quality", 90, AV_OPT_SEARCH_CHILDREN);
        /* 高频变化画面下使用更短 GOP，减少错误传播时长 */
        m_codecContext->gop_size = std::max(1, m_fps / 2);
        m_codecContext->keyint_min = std::max(1, m_fps / 4);
        if (codecInfo->isHardware)
        {
            LOG_INFO("MediaFoundation encoder supports hardware acceleration, applying hwaccel-specific options");
            /* 强制使用硬件编码（如可用） */
            av_opt_set_int(m_codecContext->priv_data, "hw_encoding", 1, AV_OPT_SEARCH_CHILDREN);
            /* 确保使用 NV12，避免额外颜色空间转换导致模糊 */
            m_codecContext->pix_fmt = AV_PIX_FMT_NV12;
        }
    }
    else if (isVideoToolbox)
    {
        LOG_INFO("Applying VideoToolbox-specific encoder options for {}", codecInfo->toString());
        av_opt_set(m_codecContext->priv_data, "realtime", "1", AV_OPT_SEARCH_CHILDREN);
        av_opt_set(m_codecContext->priv_data, "allow_sw", "1", AV_OPT_SEARCH_CHILDREN);
        av_opt_set(m_codecContext->priv_data, "prio_speed", "1", AV_OPT_SEARCH_CHILDREN);
        m_codecContext->gop_size = std::max(1, m_fps);
        m_codecContext->keyint_min = std::max(1, m_fps / 2);
        m_codecContext->pix_fmt = AV_PIX_FMT_NV12;
    }
    else if (isVaapi)
    {
        LOG_INFO("Applying VAAPI-specific encoder options for {}", codecInfo->toString());
        av_opt_set(m_codecContext->priv_data, "rc_mode", "CBR", AV_OPT_SEARCH_CHILDREN);
        av_opt_set(m_codecContext->priv_data, "low_power", "1", AV_OPT_SEARCH_CHILDREN);
        av_opt_set(m_codecContext->priv_data, "idr_interval", "1", AV_OPT_SEARCH_CHILDREN);
        m_codecContext->pix_fmt = AV_PIX_FMT_NV12;
    }
    else if (isQsv)
    {
        LOG_INFO("Applying QSV-specific encoder options for {}", codecInfo->toString());
        av_opt_set(m_codecContext->priv_data, "async_depth", "1", 0);
        av_opt_set(m_codecContext->priv_data, "look_ahead", "0", 0);
        av_opt_set(m_codecContext->priv_data, "bf", "0", 0);
    }
    else if (isNvenc)
    {
        LOG_INFO("Applying NVENC-specific encoder options for {}", codecInfo->toString());
        av_opt_set(m_codecContext->priv_data, "delay", "0", AV_OPT_SEARCH_CHILDREN);
        av_opt_set(m_codecContext->priv_data, "forced-idr", "1", AV_OPT_SEARCH_CHILDREN);
        av_opt_set(m_codecContext->priv_data, "tune", "ll", AV_OPT_SEARCH_CHILDREN);
        av_opt_set(m_codecContext->priv_data, "rc", "cbr", AV_OPT_SEARCH_CHILDREN);
        av_opt_set(m_codecContext->priv_data, "zerolatency", "1", AV_OPT_SEARCH_CHILDREN);
    }
    else if (isAmf)
    {
        LOG_INFO("Applying AMF-specific encoder options for {}", codecInfo->toString());
        av_opt_set(m_codecContext->priv_data, "usage", "ultralowlatency", AV_OPT_SEARCH_CHILDREN);
        av_opt_set(m_codecContext->priv_data, "quality", "balanced", AV_OPT_SEARCH_CHILDREN);
        m_codecContext->pix_fmt = AV_PIX_FMT_NV12;
    }
    /* 设置编码预设和调优。MediaFoundation 硬编使用系统内存输入，不需要 FFmpeg hw_frames_ctx。 */
    if (codecInfo->isHardware && isMediaFoundation)
    {
        m_hwDeviceCtx = nullptr;
        LOG_INFO("Initialized MediaFoundation hardware encoding without hwframe context ({}x{}, {}fps, {}bps)",
                 m_dstW, m_dstH, m_fps, m_bitrate);
    }
    else if (codecInfo->isHardware)
    {
        if (!initializeHardwareAccel(codecInfo))
        {
            LOG_ERROR("Failed to initialize {}", codecInfo->toString());
            return false;
        }
    }
    else
    {
        /* 软件编码优化 */
        m_hwDeviceCtx = nullptr;
        /* libopenh264 在复杂场景下更容易出现块效应，使用更保守参数 */
        if (isOpenH264)
        {
            m_codecContext->thread_count = 1;
            m_codecContext->slices = 1;
            m_codecContext->gop_size = std::max(1, m_fps / 3);
            m_codecContext->keyint_min = std::max(1, m_fps / 4);
            av_opt_set_int(m_codecContext->priv_data, "allow_skip_frames", 0, AV_OPT_SEARCH_CHILDREN);
            LOG_INFO("Initialized software encoding (libopenh264) with conservative settings {}x{}, {}fps, {}bps", m_dstW, m_dstH, m_fps, m_bitrate);
        }

        /* 仅对 libx264 设置 x264 特定参数；避免将 x264 参数传给 libopenh264 等不兼容编码器 */
        if (isX264)
        {
            /* 基础编码选项 */
            av_opt_set(m_codecContext->priv_data, "nal-hrd", "cbr", AV_OPT_SEARCH_CHILDREN);
            const int vbvMaxrateKbps = std::max(500, m_bitrate / 1000);
            const int vbvBufsizeKbps = std::max(500, m_bitrate / 2000);
            /* 软编码走严格 CBR + VBV，避免 CRF 在复杂桌面场景造成瞬时码率突刺和局部花屏 */
            QString x264Params = QString("keyint=%1:min-keyint=%2:scenecut=0:repeat-headers=1:bframes=0:b-adapt=0:rc-lookahead=0:force-cfr=1:nal-hrd=cbr:vbv-maxrate=%3:vbv-bufsize=%4")
                                     .arg(m_fps)
                                     .arg(std::max(1, m_fps / 2))
                                     .arg(vbvMaxrateKbps)
                                     .arg(vbvBufsizeKbps);
            av_opt_set(m_codecContext->priv_data, "x264-params", x264Params.toUtf8().constData(), AV_OPT_SEARCH_CHILDREN);
            av_opt_set(m_codecContext->priv_data, "preset", "faster", AV_OPT_SEARCH_CHILDREN);
            av_opt_set(m_codecContext->priv_data, "tune", "zerolatency", AV_OPT_SEARCH_CHILDREN);
            /* 使用短 GOP（允许 P 帧，避免 gop_size=1） */
            m_codecContext->gop_size = m_fps; /* 1秒一个 I 帧 */
            m_codecContext->keyint_min = std::max(1, m_fps / 2);
            LOG_INFO("Initialized software encoding (libx264) with {}x{}, {}fps, {}bps", m_dstW, m_dstH, m_fps, m_bitrate);
        }
        else if (!isOpenH264)
        {
            /* 其他软件编码器使用更通用的设置 */
            m_codecContext->gop_size = m_fps; /* 1秒一个 I 帧 */
            m_codecContext->keyint_min = std::max(1, m_fps / 2);
            LOG_INFO("Initialized software encoding (codec={}) with {}x{}, {}fps, {}bps", codecInfo->name, m_dstW, m_dstH, m_fps, m_bitrate);
        }
    }

    /* 仅当编码器支持多线程且不是 openh264 时使用多线程，否则强制单线程以避免兼容性/画面问题 */
    /* x264 在远程桌面场景下使用 frame threads 时更容易出现恢复慢/局部花屏，强制单线程更稳。 */
    if (isX264)
    {
        m_codecContext->thread_count = 1;
        m_codecContext->thread_type = 0;
        LOG_DEBUG("Codec {} forced to single-threaded encode for stability", codecInfo->name);
    }
    else if (!isOpenH264 && codecInfo->codec && (codecInfo->codec->capabilities & (AV_CODEC_CAP_FRAME_THREADS | AV_CODEC_CAP_SLICE_THREADS)))
    {
        m_codecContext->thread_count = QThread::idealThreadCount();
        m_codecContext->thread_type = FF_THREAD_FRAME;
    }
    else
    {
        m_codecContext->thread_count = 1;
        LOG_DEBUG("Codec {} forced to single-threaded encode", codecInfo->name);
    }

    /* 打开编码器 */
    int ret = avcodec_open2(m_codecContext, codecInfo->codec, nullptr);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Could not open codec {} ({}x{}, {}fps, {}bps): {} (error code: {})",
                  codecInfo->name, m_dstW, m_dstH, m_fps, m_bitrate, errbuf, ret);
        return false;
    }

    /* 对于软件编码器，确保后续的像素格式转换目标匹配编码器期望的像素格式 */
    if (!codecInfo->isHardware)
    {
        m_targetSwFmt = m_codecContext->pix_fmt;
        LOG_DEBUG("Software codec selected target sw format: {}", av_get_pix_fmt_name(m_targetSwFmt));
    }

    LOG_INFO("Encoder opened: codec={}, hw={}, pix_fmt={}, target_sw_fmt={}, {}x{}, fps={}, bitrate={}",
             codecInfo->name,
             codecInfo->isHardware,
             av_get_pix_fmt_name(m_codecContext->pix_fmt),
             av_get_pix_fmt_name(m_targetSwFmt),
             m_dstW,
             m_dstH,
             m_fps,
             m_bitrate);

    /* 分配帧 */
    m_frame = av_frame_alloc();
    if (!m_frame)
    {
        LOG_ERROR("Could not allocate video frame");
        return false;
    }

    /* 所有编码器都统一分配buffer，包括QSV */
    m_frame->format = m_codecContext->pix_fmt;
    m_frame->width = m_codecContext->width;
    m_frame->height = m_codecContext->height;

    if (!codecInfo->isHardware)
    {
        /* 只有软件编码模式才需要分配系统内存 Buffer */
        ret = av_frame_get_buffer(m_frame, 32);
        if (ret < 0)
        {
            LOG_ERROR("Could not allocate video frame data (Software Mode)");
            return false;
        }
    }
    else
    {
        /* 硬件模式下，m_frame 在这里只需要作为一个占位符 */
        LOG_INFO("Hardware mode: skipping manual frame buffer allocation.");
    }

    /* 分配数据包 */
    m_packet = av_packet_alloc();
    if (!m_packet)
    {
        LOG_ERROR("Could not allocate packet");
        return false;
    }
    return true;
}
