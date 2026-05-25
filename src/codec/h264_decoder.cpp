/* Split from h264_decoder.cpp by decoder responsibility. */

#include "h264_decoder.h"
#include "hardware_context_manager.h"

H264Decoder::H264Decoder(QObject *parent)
    : QObject(parent), m_codecContext(nullptr), m_hwDeviceCtx(nullptr), m_initialized(false), m_lastDecodeHadError(false)
{
}

H264Decoder::~H264Decoder()
{
    QMutexLocker locker(&m_mutex);
    cleanup();
}

bool H264Decoder::initialize()
{
    QMutexLocker locker(&m_mutex);

    if (m_initialized)
    {
        LOG_INFO("Decoder already initialized");
        return true;
    }

    /* 逐一尝试硬件加速器 */
    for (const auto &codecInfo : FFmpegUtil->getH264Decoders())
    {
        cleanup();
        m_codecInfo = codecInfo;
        LOG_INFO("Trying acceleration: {}", codecInfo->toString());
        m_initialized = initializeCodec(codecInfo);
        if (m_initialized)
        {
            break;
        }
        LOG_WARN("Failed to initialize {} acceleration, trying next", codecInfo->name);
    }

    if (m_initialized)
    {
        LOG_INFO("Successfully initialized H264 decoder with {} acceleration", m_codecInfo->name);
    }
    else
    {
        LOG_ERROR("Failed to initialize H264 decoder with any method");
        cleanup();
    }

    return m_initialized;
}

bool H264Decoder::lastDecodeHadError() const
{
    return m_lastDecodeHadError;
}

bool H264Decoder::initializeCodec(std::shared_ptr<CodecInfo> codecInfo)
{
    m_hwAccelInfo.reset();

    /* 创建解码器上下文 */
    m_codecContext = avcodec_alloc_context3(codecInfo->codec);
    if (!m_codecContext)
    {
        LOG_ERROR("Could not allocate video codec context");
        return false;
    }

    m_codecContext->time_base = AVRational{1, ConfigUtil->fps};
    m_codecContext->framerate = AVRational{ConfigUtil->fps, 1};
    /* 智能硬件加速初始化 */
    bool hardwareInitialized = false;
    if (codecInfo->isHardware)
    {
        LOG_DEBUG("Setting hardware decoding parameters for: {}", codecInfo->name);
        if (initializeHardwareAccel(codecInfo))
        {
            /* 设置get_format回调函数，这是硬件解码的关键 */
            m_codecContext->get_format = get_hw_format;
            m_codecContext->opaque = this; /* 传递this指针给回调函数 */
            hardwareInitialized = true;
            LOG_DEBUG("Hardware acceleration setup completed for: {}", codecInfo->name);
        }
        else
        {
            LOG_WARN("Hardware acceleration setup failed for: {}", codecInfo->name);
            return false;
        }
    }

    /* 打开解码器（硬件或软件） */
    int ret = avcodec_open2(m_codecContext, codecInfo->codec, nullptr);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Software decoder failed to open: {}", errbuf);
        return false;
    }

    /* 分配视频帧 */
    AVFrame *frame = av_frame_alloc();
    if (!frame)
    {
        LOG_ERROR("Could not allocate video frame");
        return false;
    }
    av_frame_free(&frame);
    /* 分配数据包 */
    AVPacket *packet = av_packet_alloc();
    if (!packet)
    {
        LOG_ERROR("Could not allocate packet");
        return false;
    }
    av_packet_free(&packet);
    LOG_INFO("Decoder initialization completed for: {}", codecInfo->name);
    return true;
}

bool H264Decoder::initializeHardwareAccel(std::shared_ptr<CodecInfo> codecInfo)
{
    /* 使用共享的硬件设备上下文管理 */
    LOG_INFO("Getting shared hardware device context for: {}", codecInfo->name);

    const QList<std::shared_ptr<HwAccelInfo>> &candidates = codecInfo->supportedHwTypes;

    for (const auto &hwaccelInfo : candidates)
    {
        if (!hwaccelInfo)
            continue;

        if (m_disabledHwTypes.contains(static_cast<int>(hwaccelInfo->hwDeviceType)))
        {
            LOG_WARN("Skipping blacklisted hw backend {} for decoder {}",
                     hwaccelInfo->hwDeviceTypeName,
                     codecInfo->name);
            continue;
        }

        m_hwDeviceCtx = HardwareContextManager::instance().getDeviceContext(hwaccelInfo->hwDeviceType);

        if (m_hwDeviceCtx)
        {
            m_hwAccelInfo = hwaccelInfo;
            break;
        }
        LOG_ERROR("Failed to create/get hardware device context for {}", hwaccelInfo->hwDeviceTypeName);
    }
    if (!m_hwDeviceCtx)
    {
        LOG_ERROR("Failed to obtain hardware device context for: {}", codecInfo->name);
        return false;
    }
    /* 将硬件设备上下文分配给解码器上下文 */
    m_codecContext->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    LOG_INFO("Successfully assigned hardware device {} to decoder", m_hwAccelInfo->hwDeviceTypeName);
    return true;
}

int H264Decoder::sendPacketToDecoder(AVPacket *packet)
{
#if AIRAN_FFMPEG_HAS_SEND_RECEIVE_API
    return avcodec_send_packet(m_codecContext, packet);
#else
    Q_UNUSED(packet);
    return 0;
#endif
}

int H264Decoder::receiveFrameFromDecoder(AVFrame *frame)
{
#if AIRAN_FFMPEG_HAS_SEND_RECEIVE_API
    return avcodec_receive_frame(m_codecContext, frame);
#else
    return AVERROR_EOF;
#endif
}
