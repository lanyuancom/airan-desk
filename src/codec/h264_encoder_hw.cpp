#include "h264_encoder.h"
#include "hardware_context_manager.h"

#include <QFile>
#include <cstdlib>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

#ifndef AV_PIX_FMT_D3D11
#define AV_PIX_FMT_D3D11 AV_PIX_FMT_D3D11VA_VLD
#endif

bool H264Encoder::initializeHardwareAccel(std::shared_ptr<CodecInfo> codecInfo)
{
    if (!m_codecContext)
    {
        LOG_ERROR("initializeHardwareAccel called with null codec context");
        return false;
    }
    const bool isAmf = codecInfo && codecInfo->name.contains("amf", Qt::CaseInsensitive);
    const bool isVideoToolbox = codecInfo && codecInfo->name.contains("videotoolbox", Qt::CaseInsensitive);
    const bool isVaapi = codecInfo && codecInfo->name.contains("vaapi", Qt::CaseInsensitive);
    int ret = 0;
    for (const auto &hwaccelInfo : codecInfo->supportedHwTypes)
    {
        if (hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_NONE)
        {
            LOG_WARN("Skipping unsupported hardware device type NONE");
            continue;
        }

        /* 某些 Windows + 驱动 + FFmpeg 组合下，Vulkan H264 编码链路稳定性较差，直接跳过避免崩溃。 */
        if (codecInfo->name.contains("vulkan", Qt::CaseInsensitive) || hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_VULKAN)
        {
            LOG_WARN("Skipping unstable Vulkan hardware encoder path for {}", codecInfo->name);
            continue;
        }

        /* AMF 在当前链路下走 DXVA2 易出现严重花屏，优先仅走 D3D11VA。 */
        if (isAmf && hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_DXVA2)
        {
            LOG_WARN("Skipping DXVA2 hardware device for AMF encoder {} due to artifact risk", codecInfo->name);
            continue;
        }
        /* 设置硬编码器特有参数 */
        switch (hwaccelInfo->hwDeviceType)
        {
        case AIRAN_AV_HWDEVICE_TYPE_QSV:
            /* QSV 常用稳定参数 */
            av_opt_set(m_codecContext->priv_data, "async_depth", "1", 0);
            av_opt_set(m_codecContext->priv_data, "look_ahead", "0", 0);
            av_opt_set(m_codecContext->priv_data, "b", "0", 0);
            av_opt_set(m_codecContext->priv_data, "bf", "0", 0);
            av_opt_set(m_codecContext->priv_data, "repeat-headers", "1", 0);
            av_opt_set(m_codecContext->priv_data, "idr_interval", "1", 0);
            break;
        case AIRAN_AV_HWDEVICE_TYPE_VAAPI:
            av_opt_set(m_codecContext->priv_data, "rc_mode", "CBR", AV_OPT_SEARCH_CHILDREN);
            av_opt_set(m_codecContext->priv_data, "low_power", "1", AV_OPT_SEARCH_CHILDREN);
            av_opt_set(m_codecContext->priv_data, "idr_interval", "1", AV_OPT_SEARCH_CHILDREN);
            break;
        case AIRAN_AV_HWDEVICE_TYPE_CUDA:
            /* 降低延迟：从 2 帧延迟降低到 0 */
            av_opt_set(m_codecContext->priv_data, "delay", "0", AV_OPT_SEARCH_CHILDREN);
            av_opt_set(m_codecContext->priv_data, "forced-idr", "1", AV_OPT_SEARCH_CHILDREN);
            av_opt_set(m_codecContext->priv_data, "tune", "ll", AV_OPT_SEARCH_CHILDREN);
            av_opt_set(m_codecContext->priv_data, "rc", "cbr", AV_OPT_SEARCH_CHILDREN);
            av_opt_set(m_codecContext->priv_data, "repeat-headers", "1", AV_OPT_SEARCH_CHILDREN);
            /* 若驱动支持，启用 0-latency（不支持会被忽略/返回错误，FFmpeg 不会因此崩） */
            av_opt_set(m_codecContext->priv_data, "zerolatency", "1", AV_OPT_SEARCH_CHILDREN);
            break;
        case AIRAN_AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
            av_opt_set(m_codecContext->priv_data, "realtime", "1", AV_OPT_SEARCH_CHILDREN);
            av_opt_set(m_codecContext->priv_data, "allow_sw", "1", AV_OPT_SEARCH_CHILDREN);
            break;
        default:
            break;
        }

        /* 尝试初始化硬编码器 */

        if (hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_QSV && m_forcedQsvDeviceCtx)
        {
            m_hwDeviceCtx = av_buffer_ref(m_forcedQsvDeviceCtx);
            LOG_INFO("Using first-frame derived QSV device context for encoder session binding");
        }
        else if (hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_D3D11VA && m_forcedD3d11DeviceCtx)
        {
            m_hwDeviceCtx = av_buffer_ref(m_forcedD3d11DeviceCtx);
            LOG_INFO("Using first-frame bound D3D11VA device context for NVENC session binding");
        }
        else
        {
            m_hwDeviceCtx = HardwareContextManager::instance().getDeviceContext(hwaccelInfo->hwDeviceType);
        }
        if (!m_hwDeviceCtx)
        {
            LOG_ERROR("Failed to create/get hardware device context for {}", hwaccelInfo->hwDeviceTypeName);
            continue;
        }
        m_hwaccelInfo = hwaccelInfo;
        m_codecContext->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);

        AVBufferRef *hwFramesRef = nullptr;
        if (hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_QSV && m_forcedQsvFramesCtx)
        {
            m_codecContext->pix_fmt = AV_PIX_FMT_QSV;
            m_codecContext->hw_frames_ctx = av_buffer_ref(m_forcedQsvFramesCtx);
            if (m_codecContext->hw_frames_ctx)
            {
                LOG_INFO("Using forced QSV hw_frames_ctx from first graph output for encoder session");
                return true;
            }
            LOG_WARN("Failed to reference forced QSV hw_frames_ctx, fallback to normal hwframe init");
        }
        else if (hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_D3D11VA && m_forcedD3d11FramesCtx)
        {
            m_codecContext->pix_fmt = AV_PIX_FMT_D3D11;
            m_codecContext->hw_frames_ctx = av_buffer_ref(m_forcedD3d11FramesCtx);
            if (m_codecContext->hw_frames_ctx)
            {
                LOG_INFO("Using first-frame bound D3D11VA hw_frames_ctx for NVENC session");
                return true;
            }
            LOG_WARN("Failed to reference forced D3D11VA hw_frames_ctx, fallback to normal hwframe init");
        }

        /* 先查询设备支持的 system-memory (sw) 格式，再决定 framesCtx->sw_format --- */
        AVHWFramesConstraints *constraints = av_hwdevice_get_hwframe_constraints(m_hwDeviceCtx, nullptr);
        AVPixelFormat chosenSw = AV_PIX_FMT_NONE;
        if (constraints && constraints->valid_sw_formats)
        {
            /* 收集支持的格式，优先选项：优先匹配源像素格式（如果已知），否则优先 BGRA，再 BGR0，再 NV12 */
            bool has_bgra = false;
            bool has_bgr0 = false;
            bool has_nv12 = false;
            for (int k = 0; constraints->valid_sw_formats[k] != AV_PIX_FMT_NONE; ++k)
            {
                AVPixelFormat f = constraints->valid_sw_formats[k];
                if (f == AV_PIX_FMT_BGRA)
                    has_bgra = true;
                else if (f == AV_PIX_FMT_BGR0)
                    has_bgr0 = true;
                else if (f == AV_PIX_FMT_NV12)
                    has_nv12 = true;
            }
            /* 如果我们已经知道采集源格式，优先匹配 */

            if (hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_D3D11VA ||
                hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_D3D12VA ||
                hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_DXVA2)
            {
                if (isAmf && has_nv12)
                {
                    chosenSw = AV_PIX_FMT_NV12;
                }
                else if (has_bgra)
                {
                    chosenSw = AV_PIX_FMT_BGRA;
                }
                else if (has_bgr0)
                {
                    chosenSw = AV_PIX_FMT_BGR0;
                }
                else if (has_nv12)
                {
                    chosenSw = AV_PIX_FMT_NV12;
                }
            }
            else if (isVideoToolbox && has_nv12)
                chosenSw = AV_PIX_FMT_NV12;
            else if (isVaapi && has_nv12)
                chosenSw = AV_PIX_FMT_NV12;
            else if (has_bgra)
                chosenSw = AV_PIX_FMT_BGRA; /* 更常见且避免通道重排 */
            else if (has_bgr0)
                chosenSw = AV_PIX_FMT_BGR0;
            else if (has_nv12)
                chosenSw = AV_PIX_FMT_NV12;
        }
        else if (!constraints)
        {
            LOG_WARN("Could not query hwframe constraints for {}, fallback sw_format=nv12", hwaccelInfo->hwDeviceTypeName);
        }
        if (chosenSw == AV_PIX_FMT_NONE)
            chosenSw = AV_PIX_FMT_NV12; /* 兜底 */
        if (constraints)
        {
            av_hwframe_constraints_free(&constraints);
        }
        /* 记录诊断日志 */
        LOG_DEBUG("Device {} valid sw_format chosen: {}", hwaccelInfo->hwDeviceTypeName, av_get_pix_fmt_name(chosenSw));
        /* 保存为成员，后续 capturePath 使用 */
        m_targetSwFmt = chosenSw;

        /* 初始化硬编码器像素格式 */
        int i = 0;
        for (i = 0; i < hwaccelInfo->supportedPixFormats.size(); i++)
        {
            AVPixelFormat pixelFormatTmp = hwaccelInfo->supportedPixFormats[i];
            if (pixelFormatTmp == AV_PIX_FMT_NONE)
            {
                LOG_WARN("Skipping unsupported pixel format NONE for {}", hwaccelInfo->supportedPixFormatNames[i]);
                continue;
            }

            /* dxva2_vld/d3d11va_vld 属于解码输出格式，不能作为编码输入格式使用。 */
            if (pixelFormatTmp == AV_PIX_FMT_DXVA2_VLD
#ifdef AV_PIX_FMT_D3D11VA_VLD
                || pixelFormatTmp == AV_PIX_FMT_D3D11VA_VLD
#endif
            )
            {
                LOG_WARN("Skipping decoder-only pixel format {} for encoder {}",
                         hwaccelInfo->supportedPixFormatNames[i], codecInfo->name);
                continue;
            }

            hwFramesRef = av_hwframe_ctx_alloc(m_hwDeviceCtx);
            if (!hwFramesRef)
            {
                LOG_ERROR("Failed to allocate hwframe context for {}", hwaccelInfo->hwDeviceTypeName);
                continue;
            }

            AVHWFramesContext *framesCtx = reinterpret_cast<AVHWFramesContext *>(hwFramesRef->data);
            framesCtx->sw_format = chosenSw;
            framesCtx->width = m_codecContext->width;
            framesCtx->height = m_codecContext->height;
            framesCtx->initial_pool_size = 20;
            framesCtx->format = pixelFormatTmp;

            m_codecContext->pix_fmt = pixelFormatTmp;
            ret = av_hwframe_ctx_init(hwFramesRef);
            if (ret < 0)
            {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("Failed to init hwframe context for {}: {}", hwaccelInfo->supportedPixFormatNames[i], errbuf);
                av_buffer_unref(&hwFramesRef);
                continue;
            }
            LOG_INFO("Initialized hwframe context for {} with pix_fmt={}", hwaccelInfo->hwDeviceTypeName, hwaccelInfo->supportedPixFormatNames[i]);
            break;
        }
        if (i == hwaccelInfo->supportedPixFormats.size())
        {
            continue;
        }
        m_codecContext->hw_frames_ctx = hwFramesRef;

        LOG_INFO("Successfully initialized hardware acceleration with {} on {}", codecInfo->name, hwaccelInfo->hwDeviceTypeName);
        return true;
    }
    LOG_ERROR("Failed to initialize hwframe context for all supported pixel formats on {}", codecInfo->name);
    return false;
}

/* 新增：通用系统内存像素格式转换 */
