/* Windows zero-copy H264 encoder path split by responsibility. */

#include "h264_encoder.h"
#include "common/platform_headers.h"

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
extern "C"
{
#include <libavutil/hwcontext_d3d11va.h>
}

#ifndef AV_PIX_FMT_D3D11
#define AV_PIX_FMT_D3D11 AV_PIX_FMT_D3D11VA_VLD
#endif

bool H264Encoder::reinitializeQsvSessionForZeroCopy()
{
    if (!m_codecInfo || !m_hwaccelInfo || m_hwaccelInfo->hwDeviceType != AIRAN_AV_HWDEVICE_TYPE_QSV)
        return true;

    if (!m_d3d11vaDeviceCtx)
    {
        LOG_ERROR("QSV session reinit skipped: d3d11va device ctx is null");
        return false;
    }

    AVBufferRef *derivedQsvCtx = nullptr;
    int deriveRet = av_hwdevice_ctx_create_derived(&derivedQsvCtx,
                                                   AIRAN_AV_HWDEVICE_TYPE_QSV,
                                                   m_d3d11vaDeviceCtx,
                                                   0);
    if (deriveRet < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(deriveRet, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to derive first-frame QSV device from d3d11va: {}", errbuf);
        return false;
    }

    if (m_filterGraph)
        avfilter_graph_free(&m_filterGraph);
    m_filterGraph = nullptr;
    m_bufferSrcCtx = nullptr;
    m_bufferSinkCtx = nullptr;
    if (m_filterFramesCtx)
        av_buffer_unref(&m_filterFramesCtx);
    m_filterFramesCtx = nullptr;
    m_filterSrcW = 0;
    m_filterSrcH = 0;
    m_filterNeedScale = false;

    if (m_packet)
        av_packet_free(&m_packet);
    m_packet = nullptr;
    if (m_frame)
        av_frame_free(&m_frame);
    m_frame = nullptr;
    if (m_hwFrame)
        av_frame_free(&m_hwFrame);
    m_hwFrame = nullptr;
    if (m_swsContext)
        sws_freeContext(m_swsContext);
    m_swsContext = nullptr;
    if (m_codecContext)
        avcodec_free_context(&m_codecContext);
    m_codecContext = nullptr;
    if (m_hwDeviceCtx)
        av_buffer_unref(&m_hwDeviceCtx);
    m_hwDeviceCtx = nullptr;
    if (m_forcedQsvDeviceCtx)
        av_buffer_unref(&m_forcedQsvDeviceCtx);
    if (m_forcedQsvFramesCtx)
        av_buffer_unref(&m_forcedQsvFramesCtx);
    m_forcedQsvFramesCtx = nullptr;
    m_forcedQsvDeviceCtx = derivedQsvCtx;

    bool ok = initializeCodec(m_codecInfo);
    if (!ok)
    {
        LOG_ERROR("Failed to reopen QSV encoder with first-frame derived session");
        return false;
    }

    m_qsvSessionBound = true;
    m_qsvFramesBound = false;
    m_initialized = true;
    LOG_INFO("QSV encoder session rebound once using first-frame D3D11 device");
    return true;
}

bool H264Encoder::reinitializeD3d11vaSessionForZeroCopy()
{
    if (!m_codecInfo || !m_hwaccelInfo || m_hwaccelInfo->hwDeviceType != AIRAN_AV_HWDEVICE_TYPE_D3D11VA)
        return true;

    if (!m_d3d11vaDeviceCtx || !m_d3d11vaFramesCtx)
    {
        LOG_ERROR("D3D11VA session reinit skipped: source D3D11VA contexts are null");
        return false;
    }

    if (m_filterGraph)
        avfilter_graph_free(&m_filterGraph);
    m_filterGraph = nullptr;
    m_bufferSrcCtx = nullptr;
    m_bufferSinkCtx = nullptr;
    if (m_filterFramesCtx)
        av_buffer_unref(&m_filterFramesCtx);
    m_filterFramesCtx = nullptr;
    m_filterSrcW = 0;
    m_filterSrcH = 0;
    m_filterNeedScale = false;

    if (m_packet)
        av_packet_free(&m_packet);
    m_packet = nullptr;
    if (m_frame)
        av_frame_free(&m_frame);
    m_frame = nullptr;
    if (m_hwFrame)
        av_frame_free(&m_hwFrame);
    m_hwFrame = nullptr;
    if (m_swsContext)
        sws_freeContext(m_swsContext);
    m_swsContext = nullptr;
    if (m_codecContext)
        avcodec_free_context(&m_codecContext);
    m_codecContext = nullptr;
    if (m_hwDeviceCtx)
        av_buffer_unref(&m_hwDeviceCtx);
    m_hwDeviceCtx = nullptr;

    if (m_forcedD3d11DeviceCtx)
        av_buffer_unref(&m_forcedD3d11DeviceCtx);
    if (m_forcedD3d11FramesCtx)
        av_buffer_unref(&m_forcedD3d11FramesCtx);

    m_forcedD3d11DeviceCtx = av_buffer_ref(m_d3d11vaDeviceCtx);
    m_forcedD3d11FramesCtx = av_buffer_ref(m_d3d11vaFramesCtx);
    if (!m_forcedD3d11DeviceCtx || !m_forcedD3d11FramesCtx)
    {
        LOG_ERROR("Failed to reference first-frame D3D11VA contexts for NVENC session binding");
        if (m_forcedD3d11DeviceCtx)
            av_buffer_unref(&m_forcedD3d11DeviceCtx);
        if (m_forcedD3d11FramesCtx)
            av_buffer_unref(&m_forcedD3d11FramesCtx);
        return false;
    }

    bool ok = initializeCodec(m_codecInfo);
    if (!ok)
    {
        LOG_ERROR("Failed to reopen NVENC encoder with first-frame D3D11VA session binding");
        return false;
    }

    m_d3d11SessionBound = true;
    m_initialized = true;
    LOG_INFO("NVENC(D3D11VA) encoder session rebound once using first-frame D3D11 device/frames context");
    return true;
}

bool H264Encoder::reinitializeQsvCodecWithGraphFrames(AVBufferRef *graphFramesCtx)
{
    if (!graphFramesCtx || !m_codecInfo || !m_hwaccelInfo || m_hwaccelInfo->hwDeviceType != AIRAN_AV_HWDEVICE_TYPE_QSV)
        return false;

    AVBufferRef *newForcedFrames = av_buffer_ref(graphFramesCtx);
    if (!newForcedFrames)
    {
        LOG_ERROR("Failed to reference graph output QSV hw_frames_ctx");
        return false;
    }

    if (m_packet)
        av_packet_free(&m_packet);
    m_packet = nullptr;
    if (m_frame)
        av_frame_free(&m_frame);
    m_frame = nullptr;
    if (m_hwFrame)
        av_frame_free(&m_hwFrame);
    m_hwFrame = nullptr;
    if (m_swsContext)
        sws_freeContext(m_swsContext);
    m_swsContext = nullptr;
    if (m_codecContext)
        avcodec_free_context(&m_codecContext);
    m_codecContext = nullptr;
    if (m_hwDeviceCtx)
        av_buffer_unref(&m_hwDeviceCtx);
    m_hwDeviceCtx = nullptr;

    if (m_forcedQsvFramesCtx)
        av_buffer_unref(&m_forcedQsvFramesCtx);
    m_forcedQsvFramesCtx = newForcedFrames;

    bool ok = initializeCodec(m_codecInfo);
    if (!ok)
    {
        LOG_ERROR("Failed to reopen QSV encoder with graph output hw_frames_ctx");
        return false;
    }

    m_qsvFramesBound = true;
    m_initialized = true;
    LOG_INFO("QSV encoder rebound once using graph output hw_frames_ctx");
    return true;
}

/* 零拷贝编码：D3D11Texture2D -> 硬件编码器 零拷贝编码 */

#endif
