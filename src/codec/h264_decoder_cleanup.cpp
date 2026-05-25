/* Split from h264_decoder.cpp by decoder responsibility. */

#include "h264_decoder.h"

void H264Decoder::cleanup()
{
    /* 标记为未初始化，阻止新的解码请求进入 */
    m_initialized = false;
    m_hwAccelInfo.reset();

    if (m_codecContext)
    {
        avcodec_free_context(&m_codecContext);
        m_codecContext = nullptr;
    }

    /* 释放硬件设备上下文的引用（共享管理器会处理实际的释放） */
    if (m_hwDeviceCtx)
    {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
    }

    m_initialized = false;

    LOG_DEBUG("H264Decoder cleanup completed");
}

/* 硬件解码的关键回调函数 - 根据FFmpeg官方示例 */
enum AVPixelFormat H264Decoder::get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    H264Decoder *decoder = static_cast<H264Decoder *>(ctx->opaque);
    if (!decoder)
    {
        LOG_ERROR("Decoder instance is null in get_hw_format callback");
        return AV_PIX_FMT_NONE;
    }
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++)
    {
        if (decoder->m_hwAccelInfo && decoder->m_hwAccelInfo->supportedPixFormats.contains(*p))
        {
            LOG_DEBUG("Selected hardware pixel format: {}", av_get_pix_fmt_name(*p));
            return *p;
        }
    }

    if (pix_fmts && pix_fmts[0] != AV_PIX_FMT_NONE)
    {
        LOG_WARN("No matching hardware pixel format found, fallback to software pixel format: {}", av_get_pix_fmt_name(pix_fmts[0]));
        return pix_fmts[0];
    }

    LOG_ERROR("No suitable pixel format found");
    return AV_PIX_FMT_NONE;
}
