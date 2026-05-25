#include "h264_encoder.h"

#include <algorithm>
#include <cstring>

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#include <d3d11.h>
#endif

bool H264Encoder::convertToSwFormat(AVFrame *inputFrame, AVFrame *outputFrame, AVPixelFormat dstFormat)
{
    if (!inputFrame || !outputFrame)
        return false;
    AVPixelFormat inputFormat = static_cast<AVPixelFormat>(inputFrame->format);
    if (inputFrame->width <= 0 || inputFrame->height <= 0)
        return false;

    /* 编码器内部不再进行缩放：输出尺寸始终等于输入尺寸 */
    int dstW = inputFrame->width;
    int dstH = inputFrame->height;

    outputFrame->format = dstFormat;
    outputFrame->width = dstW;
    outputFrame->height = dstH;

    int ret = av_frame_get_buffer(outputFrame, 32);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("convertToSwFormat: Failed to allocate buffer for {}: {}", av_get_pix_fmt_name(dstFormat), errbuf);
        return false;
    }

    /* 拷贝色彩元数据 */
    outputFrame->colorspace = inputFrame->colorspace;
    outputFrame->color_primaries = inputFrame->color_primaries;
    outputFrame->color_trc = inputFrame->color_trc;
    outputFrame->color_range = inputFrame->color_range;

    m_swsContext = sws_getCachedContext(
        m_swsContext,
        inputFrame->width, inputFrame->height, inputFormat,
        dstW, dstH, dstFormat,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsContext)
    {
        LOG_ERROR("convertToSwFormat: Failed to create sws context from {} {}x{} to {} {}x{}",
                  av_get_pix_fmt_name(inputFormat), inputFrame->width, inputFrame->height,
                  av_get_pix_fmt_name(dstFormat), dstW, dstH);
        m_srcPixFmt = AV_PIX_FMT_NONE;
        m_cachedSwsDstFmt = AV_PIX_FMT_NONE;
        return false;
    }
    m_srcPixFmt = inputFormat;
    m_cachedSwsDstFmt = dstFormat;
    m_cachedSwsSrcW = inputFrame->width;
    m_cachedSwsSrcH = inputFrame->height;
    m_cachedSwsDstW = dstW;
    m_cachedSwsDstH = dstH;

    int scaled = sws_scale(m_swsContext,
                           inputFrame->data, inputFrame->linesize, 0, inputFrame->height,
                           outputFrame->data, outputFrame->linesize);
    if (scaled != dstH)
    {
        LOG_ERROR("convertToSwFormat: sws_scale unexpected lines: expected {} got {} (input {}x{})",
                  dstH, scaled, inputFrame->width, inputFrame->height);
        return false;
    }

    LOG_TRACE("convertToSwFormat: converted {} {}x{} -> {} {}x{} linesize0={} color_range={}",
              av_get_pix_fmt_name(inputFormat), inputFrame->width, inputFrame->height,
              av_get_pix_fmt_name(dstFormat), dstW, dstH, outputFrame->linesize[0], (int)(outputFrame->color_range));
    return true;
}

void H264Encoder::cleanup()
{
    /* 1. 先释放帧、包、SwsContext */
    if (m_packet)
    {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    if (m_frame)
    {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    if (m_hwFrame)
    {
        av_frame_free(&m_hwFrame);
        m_hwFrame = nullptr;
    }
    if (m_swsContext)
    {
        sws_freeContext(m_swsContext);
        m_swsContext = nullptr;
    }
    m_srcPixFmt = AV_PIX_FMT_NONE;
    m_cachedSwsDstFmt = AV_PIX_FMT_NONE;
    m_cachedSwsSrcW = 0;
    m_cachedSwsSrcH = 0;
    m_cachedSwsDstW = 0;
    m_cachedSwsDstH = 0;
    /* 2. 释放滤镜图 */
    if (m_filterGraph)
    {
        avfilter_graph_free(&m_filterGraph);
        m_filterGraph = nullptr;
    }
    m_bufferSrcCtx = nullptr;
    m_bufferSinkCtx = nullptr;
    if (m_filterFramesCtx)
    {
        av_buffer_unref(&m_filterFramesCtx);
        m_filterFramesCtx = nullptr;
    }
    m_filterSrcW = 0;
    m_filterSrcH = 0;
    m_filterNeedScale = false;
    m_zeroCopyHealthy = true;
    m_qsvDeriveChecked = false;
    m_qsvDeriveOk = false;
    m_qsvSessionBound = false;
    m_qsvFramesBound = false;
    m_d3d11SessionBound = false;

    /* 3. 释放编码器上下文 */
    if (m_codecContext)
    {
        avcodec_free_context(&m_codecContext);
        m_codecContext = nullptr;
    }
    /* 4. 释放硬件设备上下文 */
    if (m_hwDeviceCtx)
    {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
    }
    if (m_d3d11vaDeviceCtx)
    {
        av_buffer_unref(&m_d3d11vaDeviceCtx);
        m_d3d11vaDeviceCtx = nullptr;
    }
    if (m_d3d11vaFramesCtx)
    {
        av_buffer_unref(&m_d3d11vaFramesCtx);
        m_d3d11vaFramesCtx = nullptr;
    }
    if (m_forcedQsvDeviceCtx)
    {
        av_buffer_unref(&m_forcedQsvDeviceCtx);
        m_forcedQsvDeviceCtx = nullptr;
    }
    if (m_forcedQsvFramesCtx)
    {
        av_buffer_unref(&m_forcedQsvFramesCtx);
        m_forcedQsvFramesCtx = nullptr;
    }
    if (m_forcedD3d11DeviceCtx)
    {
        av_buffer_unref(&m_forcedD3d11DeviceCtx);
        m_forcedD3d11DeviceCtx = nullptr;
    }
    if (m_forcedD3d11FramesCtx)
    {
        av_buffer_unref(&m_forcedD3d11FramesCtx);
        m_forcedD3d11FramesCtx = nullptr;
    }
    /* 5. 释放采集设备 */
    if (m_deviceCtx)
    {
        avformat_close_input(&m_deviceCtx);
    }
    if (m_codecInfo)
    {
        m_codecInfo = nullptr;
    }
    if (m_hwaccelInfo)
    {
        m_hwaccelInfo = nullptr;
    }
    m_pts = 0;
    m_codecInfo = nullptr;
    m_targetSwFmt = AV_PIX_FMT_NV12;
    m_initialized = false;

    LOG_DEBUG("H264Encoder cleanup completed");
}

/* 将 QImage 拷贝到 AVFrame (BGRA) 并转换为编码器期望的目标 sw 格式/尺寸 */
AVFrame *H264Encoder::createFrameFromQImage(const QImage &img)
{
    if (img.isNull())
        return nullptr;
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return nullptr;
    int w = img.width();
    int h = img.height();
    frame->format = AV_PIX_FMT_BGRA;
    frame->width = w;
    frame->height = h;
    int ret = av_frame_get_buffer(frame, 32);
    if (ret < 0)
    {
        av_frame_free(&frame);
        return nullptr;
    }
    /* QImage stores BGRA on little-endian for Format_ARGB32 / Format_RGB32 */
    const uchar *src = img.constBits();
    int srcStride = img.bytesPerLine();
    int dstStride = frame->linesize[0];
    for (int y = 0; y < h; ++y)
    {
        memcpy(frame->data[0] + y * dstStride, src + y * srcStride, std::min(srcStride, dstStride));
    }
    return frame;
}

void H264Encoder::appendAvailablePackets(std::shared_ptr<rtc::binary> &out, quint64 &timestampUs)
{
    if (!m_codecContext || !m_packet || !out)
        return;

#if AIRAN_FFMPEG_HAS_SEND_RECEIVE_API
    while (true)
    {
        int ret = avcodec_receive_packet(m_codecContext, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Error receiving packet from encoder: {}", errbuf);
            break;
        }

        if (m_packet->size > 0)
        {
            size_t old = out->size();
            out->resize(old + m_packet->size);
            memcpy(out->data() + old, m_packet->data, m_packet->size);
            int64_t pts = m_packet->pts;
            timestampUs = av_rescale_q(pts, m_codecContext->time_base, AVRational{1, 1000000});
        }
        av_packet_unref(m_packet);
    }
#else
    Q_UNUSED(out);
    Q_UNUSED(timestampUs);
#endif
}

int H264Encoder::sendFrameToEncoder(AVFrame *frame, std::shared_ptr<rtc::binary> &out, quint64 &timestampUs)
{
    if (!m_codecContext || !frame)
        return AVERROR(EINVAL);

#if AIRAN_FFMPEG_HAS_SEND_RECEIVE_API
    return avcodec_send_frame(m_codecContext, frame);
#else
    int gotPacket = 0;
    int ret = avcodec_encode_video2(m_codecContext, m_packet, frame, &gotPacket);
    if (ret < 0)
        return ret;

    if (gotPacket && m_packet && m_packet->size > 0 && out)
    {
        const size_t oldSize = out->size();
        out->resize(oldSize + m_packet->size);
        memcpy(out->data() + oldSize, m_packet->data, m_packet->size);
        timestampUs = av_rescale_q(m_packet->pts, m_codecContext->time_base, AVRational{1, 1000000});
        av_packet_unref(m_packet);
    }
    return 0;
#endif
}

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
/* Helper: map D3D11 texture to system memory via staging copy, return AVFrame (BGRA) */
AVFrame *H264Encoder::createFrameFromD3D11Texture(ID3D11Texture2D *tex)
{
    if (!tex)
        return nullptr;
    ID3D11Device *dev = nullptr;
    tex->GetDevice(&dev);
    if (!dev)
        return nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx)
    {
        dev->Release();
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    /* create staging texture */
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    ID3D11Texture2D *staging = nullptr;
    HRESULT hr = dev->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr) || !staging)
    {
        ctx->Release();
        dev->Release();
        return nullptr;
    }
    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        staging->Release();
        ctx->Release();
        dev->Release();
        return nullptr;
    }
    /* create AVFrame BGRA and copy */
    AVFrame *frame = av_frame_alloc();
    if (!frame)
    {
        ctx->Unmap(staging, 0);
        staging->Release();
        ctx->Release();
        dev->Release();
        return nullptr;
    }
    frame->format = AV_PIX_FMT_BGRA;
    frame->width = desc.Width;
    frame->height = desc.Height;
    if (av_frame_get_buffer(frame, 32) < 0)
    {
        av_frame_free(&frame);
        ctx->Unmap(staging, 0);
        staging->Release();
        ctx->Release();
        dev->Release();
        return nullptr;
    }
    int srcStride = mapped.RowPitch;
    int dstStride = frame->linesize[0];
    for (UINT y = 0; y < desc.Height; ++y)
    {
        memcpy(frame->data[0] + y * dstStride,
               static_cast<uint8_t *>(mapped.pData) + y * srcStride,
               static_cast<size_t>(std::min(srcStride, dstStride)));
    }
    ctx->Unmap(staging, 0);
    staging->Release();
    ctx->Release();
    dev->Release();
    return frame;
}

std::pair<std::shared_ptr<rtc::binary>, quint64> H264Encoder::encodeGPU(ID3D11Texture2D *in)
{
    QMutexLocker locker(&m_mutex);
    std::shared_ptr<rtc::binary> out = std::make_shared<rtc::binary>();
    quint64 timestampUs = 0;
    if (!m_codecContext || !in)
        return {out, timestampUs};

    /* try best-effort: map texture to CPU (staging) then convert/upload */
    AVFrame *frame = createFrameFromD3D11Texture(in);
    if (!frame)
        return {out, timestampUs};

    if (frame->width != m_dstW || frame->height != m_dstH)
    {
        LOG_ERROR("encodeGPU size mismatch: input={}x{}, encoder={}x{}. Internal scaling is disabled; scale in capture path.",
                  frame->width, frame->height, m_dstW, m_dstH);
        av_frame_free(&frame);
        return {out, timestampUs};
    }

    /* convert to target sw fmt/size */
    AVFrame *targetFrame = av_frame_alloc();
    if (!convertToSwFormat(frame, targetFrame, m_targetSwFmt))
    {
        av_frame_free(&frame);
        av_frame_free(&targetFrame);
        return {out, timestampUs};
    }
    av_frame_free(&frame);

    int ret = 0;
    if (m_codecInfo && m_codecInfo->isHardware && m_hwDeviceCtx && m_codecContext->hw_frames_ctx)
    {
        AVFrame *hwFrame = av_frame_alloc();
        if (av_hwframe_get_buffer(m_codecContext->hw_frames_ctx, hwFrame, 0) >= 0)
        {
            if (av_hwframe_transfer_data(hwFrame, targetFrame, 0) >= 0)
            {
                prepareFrameForEncode(hwFrame);
                ret = sendFrameToEncoder(hwFrame, out, timestampUs);
                av_frame_free(&hwFrame);
            }
            else
            {
                av_frame_free(&hwFrame);
                prepareFrameForEncode(targetFrame);
                ret = sendFrameToEncoder(targetFrame, out, timestampUs);
            }
        }
        else
        {
            prepareFrameForEncode(targetFrame);
            ret = sendFrameToEncoder(targetFrame, out, timestampUs);
        }
    }
    else
    {
        prepareFrameForEncode(targetFrame);
        ret = sendFrameToEncoder(targetFrame, out, timestampUs);
    }

    av_frame_free(&targetFrame);

    if (ret == AVERROR(EAGAIN))
    {
        /* 编码器输出未及时取走导致 send 被背压：先排空可用包，本帧跳过 */
        appendAvailablePackets(out, timestampUs);
        ret = AVERROR(EAGAIN);
    }

    if (ret < 0)
    {
        if (ret == AVERROR(EAGAIN))
        {
            LOG_DEBUG("Encoder still back-pressured after retry in GPU path, skip this frame");
            return {out, timestampUs};
        }
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Error sending frame to encoder (GPU path): {}", errbuf);
        return {out, timestampUs};
    }

    appendAvailablePackets(out, timestampUs);
    return {out, timestampUs};
}
#endif

/* 将已经准备好的系统内存帧（任意pixfmt/尺寸）处理并送入编码器，返回编码输出 */
std::pair<std::shared_ptr<rtc::binary>, quint64> H264Encoder::encodeCPU(const QImage &in)
{
    QMutexLocker locker(&m_mutex);
    std::shared_ptr<rtc::binary> out = std::make_shared<rtc::binary>();
    quint64 timestampUs = 0;
    if (!m_codecContext)
        return {out, timestampUs};
    AVFrame *inputFrame = createFrameFromQImage(in);
    if (!inputFrame)
        return {out, timestampUs};

    if (inputFrame->width != m_dstW || inputFrame->height != m_dstH)
    {
        LOG_ERROR("encodeCPU size mismatch: input={}x{}, encoder={}x{}. Internal scaling is disabled; scale in capture path.",
                  inputFrame->width, inputFrame->height, m_dstW, m_dstH);
        av_frame_free(&inputFrame);
        return {out, timestampUs};
    }

    int ret = 0;
    AVFrame *processedFrame = nullptr;

    /* 统一先做软件像素格式/尺寸转换，避免分支重复 */
    AVFrame *targetFrame = av_frame_alloc();
    if (!targetFrame)
    {
        av_frame_free(&inputFrame);
        return {out, timestampUs};
    }
    if (!convertToSwFormat(inputFrame, targetFrame, m_targetSwFmt))
    {
        av_frame_free(&inputFrame);
        av_frame_free(&targetFrame);
        return {out, timestampUs};
    }
    av_frame_free(&inputFrame);

    /* 若当前是硬件编码器，优先上传到硬件帧；失败则回退直送 targetFrame */
    bool tryHwUpload = (m_codecInfo && m_codecInfo->isHardware && m_hwDeviceCtx && m_codecContext->hw_frames_ctx);
    if (tryHwUpload)
    {
        AVFrame *hwFrame = av_frame_alloc();
        if (hwFrame && av_hwframe_get_buffer(m_codecContext->hw_frames_ctx, hwFrame, 0) >= 0)
        {
            if (av_hwframe_transfer_data(hwFrame, targetFrame, 0) >= 0)
            {
                processedFrame = hwFrame;
                av_frame_free(&targetFrame);
                LOG_DEBUG("encodeCPU path: software scale + hardware upload + encode");
            }
            else
            {
                av_frame_free(&hwFrame);
                processedFrame = targetFrame;
                LOG_WARN("Hardware frame upload failed, fallback to direct frame send");
            }
        }
        else
        {
            if (hwFrame)
                av_frame_free(&hwFrame);
            processedFrame = targetFrame;
            LOG_WARN("Hardware frame buffer alloc failed, fallback to direct frame send");
        }
    }
    else
    {
        processedFrame = targetFrame;
        LOG_TRACE("encodeCPU path: direct frame send");
    }

    /* 发送帧到编码器 */
    if (processedFrame)
    {
        prepareFrameForEncode(processedFrame);
        ret = sendFrameToEncoder(processedFrame, out, timestampUs);
        av_frame_free(&processedFrame);

        if (ret == AVERROR(EAGAIN))
        {
            appendAvailablePackets(out, timestampUs);
            LOG_DEBUG("Encoder back-pressured in CPU path, drained packets and skipped current frame");
            return {out, timestampUs};
        }

        if (ret < 0)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Error sending frame to encoder: {}", errbuf);
            return {out, timestampUs};
        }
    }
    else
    {
        return {out, timestampUs};
    }

    /* 接收编码后的数据包 */
    appendAvailablePackets(out, timestampUs);
    return {out, timestampUs};
}
