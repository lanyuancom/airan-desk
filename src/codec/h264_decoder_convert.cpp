/* Split from h264_decoder.cpp by decoder responsibility. */

#include "h264_decoder.h"

void H264Decoder::flushBuffers()
{
    QMutexLocker locker(&m_mutex);
    if (!m_initialized || !m_codecContext)
        return;

    avcodec_flush_buffers(m_codecContext);
    LOG_INFO("H264 decoder buffers flushed; waiting for next IDR to rebuild references");
}

bool H264Decoder::convertToTargetFmt(AVFrame *inputFrame, AVFrame *outputFrame)
{
    if (!inputFrame || !outputFrame)
    {
        return false;
    }
    /* 基本合法性检查 */
    if (inputFrame->width <= 0 || inputFrame->height <= 0)
    {
        LOG_ERROR("convertToTargetFmt: invalid input size: {}x{}", inputFrame->width, inputFrame->height);
        return false;
    }
    /* 检查 data 指针和 linesize */
    if (!inputFrame->data[0])
    {
        LOG_ERROR("convertToTargetFmt: inputFrame->data[0] is null");
        return false;
    }
    /* 如果指针看起来像小数字（非有效地址），记录并放弃转换以避免潜在的崩溃 */
    uintptr_t d0 = reinterpret_cast<uintptr_t>(inputFrame->data[0]);
    if (d0 != 0 && d0 < 0x1000)
    {
        LOG_ERROR("convertToTargetFmt: suspicious input data[0] pointer: 0x{:x}", d0);
        return false;
    }
    int srcW = inputFrame->width;
    int srcH = inputFrame->height;
    /* 基本 linesize 校验 */
    int ls0 = inputFrame->linesize[0];
    int ls1 = inputFrame->linesize[1];
    if (ls0 < srcW || ls1 < (srcW + 1) / 2)
    {
        LOG_ERROR("convertToTargetFmt: suspicious linesize: ls0={} ls1={} for width={}", ls0, ls1, srcW);
        return false;
    }

    outputFrame->format = m_codecContext->sw_pix_fmt;
    outputFrame->width = srcW;
    outputFrame->height = srcH;

    int ret = av_frame_get_buffer(outputFrame, 32);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to allocate {} frame buffer: {}", av_get_pix_fmt_name(m_codecContext->sw_pix_fmt), errbuf);
        return false;
    }
    AVPixelFormat inputFormat = static_cast<AVPixelFormat>(inputFrame->format);

    /* 创建转换上下文 */
    SwsContext *swsContext = sws_getContext(
        inputFrame->width, inputFrame->height, inputFormat,
        outputFrame->width, outputFrame->height, m_codecContext->sw_pix_fmt,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsContext)
    {
        LOG_ERROR("Failed to create sws context for {} conversion from {}", av_get_pix_fmt_name(m_codecContext->sw_pix_fmt), av_get_pix_fmt_name(inputFormat));
        return false;
    }
    /* 执行转换 */
    int scaledHeight = sws_scale(swsContext,
                                 inputFrame->data, inputFrame->linesize, 0, inputFrame->height,
                                 outputFrame->data, outputFrame->linesize);

    sws_freeContext(swsContext);

    if (scaledHeight != inputFrame->height)
    {
        LOG_ERROR("Failed to convert {} frame to {}: expected {} lines, got {}", av_get_pix_fmt_name(inputFormat), av_get_pix_fmt_name(m_codecContext->sw_pix_fmt), inputFrame->height, scaledHeight);
        return false;
    }

    LOG_DEBUG("Successfully converted {} frame to {}, videoSize: {}x{}", av_get_pix_fmt_name(inputFormat), av_get_pix_fmt_name((AVPixelFormat)outputFrame->format), outputFrame->width, outputFrame->height);
    return true;
}

QImage H264Decoder::avframeToQImage(AVFrame *frame)
{
    if (!frame)
    {
        return QImage();
    }
    /* 使用 32位对齐，渲染更快，兼容性更好 */
    const AVPixelFormat avTargetFormat = AV_PIX_FMT_RGB32;
    const QImage::Format qtImageFormat = QImage::Format_RGB32;

    /* ========================================== */
    int width = frame->width;
    int height = frame->height;
    AVPixelFormat inputFormat = static_cast<AVPixelFormat>(frame->format);

    /* 准备内存对齐的数据缓冲区，供 sws_scale 输出使用 */
    int numBytes = av_image_get_buffer_size(avTargetFormat, width, height, 32);
    if (numBytes < 0)
    {
        LOG_ERROR("Failed to get buffer size for image");
        return QImage();
    }
    uint8_t *buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
    if (!buffer)
    {
        LOG_ERROR("Failed to allocate buffer for image");
        return QImage();
    }

    /* 3. 执行转换 */
    uint8_t *dstData[4] = {0};
    int dstLinesize[4] = {0};
    av_image_fill_arrays(dstData, dstLinesize, buffer, avTargetFormat, width, height, 32);

    /* 注意：这里目标格式用 AV_PIX_FMT_RGB32 */
    SwsContext *swsContext = sws_getContext(
        width, height, inputFormat,
        width, height, avTargetFormat,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsContext)
    {
        LOG_ERROR("Failed to create sws context for AVFrame to QImage conversion");
        av_free(buffer);
        return QImage();
    }
    int result = sws_scale(swsContext,
                           frame->data, frame->linesize, 0, height,
                           dstData, dstLinesize);
    sws_freeContext(swsContext);
    if (result < 0)
    {
        av_free(buffer);
        LOG_ERROR("sws_scale failed: expected {} lines, got {}", height, result);
        return QImage();
    }
    /* 2. 创建 QImage */
    QImage image(width, height, qtImageFormat);
    /* 逐行拷贝，规避 linesize 差异和越界风险 */
    for (int i = 0; i < height; ++i)
    {
        memcpy(image.scanLine(i), dstData[0] + i * dstLinesize[0], width * 4);
    }
    av_free(buffer);
    return image;
}
