/* Windows zero-copy H264 encoder path split by responsibility. */

#include "h264_encoder.h"
#include "common/platform_headers.h"

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)

namespace
{
    struct WinVersion
    {
        DWORD major{0};
        DWORD minor{0};
    };

    WinVersion queryWindowsVersion()
    {
        WinVersion v{};
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll)
        {
            return v;
        }

        using RtlGetVersionFn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (!rtlGetVersion)
        {
            return v;
        }

        RTL_OSVERSIONINFOW osvi{};
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        if (rtlGetVersion(&osvi) == 0)
        {
            v.major = osvi.dwMajorVersion;
            v.minor = osvi.dwMinorVersion;
        }
        return v;
    }

    bool isWin8OrGreaterRuntime()
    {
        const WinVersion v = queryWindowsVersion();
        return (v.major > 6) || (v.major == 6 && v.minor >= 2);
    }
}

std::pair<std::shared_ptr<rtc::binary>, quint64> H264Encoder::zeroCopyEncodeGPU(ID3D11Texture2D *in)
{
    QMutexLocker locker(&m_mutex);
    std::shared_ptr<rtc::binary> out = std::make_shared<rtc::binary>();
    quint64 timestampUs = 0;

    if (!isWin8OrGreaterRuntime())
    {
        return {out, timestampUs};
    }

    if (!m_codecContext || !in)
        return {out, timestampUs};

    if (!m_zeroCopyHealthy)
    {
        locker.unlock();
        return encodeGPU(in);
    }

    if (!m_codecInfo || !m_codecInfo->isHardware || !m_hwaccelInfo)
    {
        LOG_WARN("zeroCopyEncodeGPU requires hardware encoder");
        return {out, timestampUs};
    }

    if (m_hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_QSV)
    {
        ID3D11Device *texDev = nullptr;
        in->GetDevice(&texDev);
        if (!texDev)
        {
            m_zeroCopyHealthy = false;
            locker.unlock();
            return encodeGPU(in);
        }

        UINT vendorId = 0;
        ComPtr<IDXGIDevice> dxgiDev;
        HRESULT hrVendor = texDev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(dxgiDev.GetAddressOf()));
        if (SUCCEEDED(hrVendor) && dxgiDev)
        {
            ComPtr<IDXGIAdapter> adapter;
            hrVendor = dxgiDev->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void **>(adapter.GetAddressOf()));
            if (SUCCEEDED(hrVendor) && adapter)
            {
                DXGI_ADAPTER_DESC desc;
                if (SUCCEEDED(adapter->GetDesc(&desc)))
                {
                    vendorId = desc.VendorId;
                }
            }
        }
        texDev->Release();

        if (vendorId != 0x8086)
        {
            LOG_WARN("Skip QSV zero-copy: source texture adapter vendor is 0x{:x} (not Intel), fallback to compatible path",
                     static_cast<unsigned int>(vendorId));
            m_zeroCopyHealthy = false;
            locker.unlock();
            return encodeGPU(in);
        }
    }

    AVFrame *d3d11Frame = createHwFrameFromD3D11Texture(in);
    if (!d3d11Frame)
    {
        LOG_ERROR("Failed to create D3D11VA frame from texture");
        m_zeroCopyHealthy = false;
        locker.unlock();
        return encodeGPU(in);
    }

    if (m_hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_QSV)
    {
        if (!m_qsvDeriveChecked)
        {
            AVBufferRef *derivedQsvCtx = nullptr;
            int deriveRet = av_hwdevice_ctx_create_derived(&derivedQsvCtx,
                                                           AIRAN_AV_HWDEVICE_TYPE_QSV,
                                                           m_d3d11vaDeviceCtx,
                                                           0);
            if (deriveRet < 0)
            {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(deriveRet, errbuf, sizeof(errbuf));
                LOG_WARN("QSV derive preflight failed (d3d11->qsv): {}. Disable zero-copy for this session", errbuf);
                m_qsvDeriveOk = false;
                m_qsvDeriveChecked = true;
            }
            else
            {
                m_qsvDeriveOk = true;
                m_qsvDeriveChecked = true;
                av_buffer_unref(&derivedQsvCtx);
                LOG_INFO("QSV derive preflight succeeded, zero-copy path enabled");
            }
        }

        if (!m_qsvDeriveOk)
        {
            m_zeroCopyHealthy = false;
            av_frame_free(&d3d11Frame);
            locker.unlock();
            return encodeGPU(in);
        }

        if (!m_qsvSessionBound)
        {
            if (!reinitializeQsvSessionForZeroCopy())
            {
                m_zeroCopyHealthy = false;
                av_frame_free(&d3d11Frame);
                locker.unlock();
                return encodeGPU(in);
            }
        }
    }

    D3D11_TEXTURE2D_DESC desc;
    in->GetDesc(&desc);

    bool needScale = (desc.Width != static_cast<UINT>(m_dstW) || desc.Height != static_cast<UINT>(m_dstH));
    if (needScale)
    {
        LOG_ERROR("zeroCopyEncodeGPU size mismatch: input={}x{}, encoder={}x{}. Internal scaling is disabled; scale in capture path.",
                  desc.Width, desc.Height, m_dstW, m_dstH);
        av_frame_free(&d3d11Frame);
        return {out, timestampUs};
    }

    if (m_hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_D3D11VA && !needScale && !m_d3d11SessionBound)
    {
        if (!reinitializeD3d11vaSessionForZeroCopy())
        {
            m_zeroCopyHealthy = false;
            av_frame_free(&d3d11Frame);
            locker.unlock();
            return encodeGPU(in);
        }
    }

    AVFrame *qsvFrame = nullptr;
    int ret = 0;

    bool needGraph = (m_hwaccelInfo->hwDeviceType != AIRAN_AV_HWDEVICE_TYPE_D3D11VA);
    if (!needGraph)
    {
        qsvFrame = d3d11Frame;
        LOG_TRACE("Zero-copy fast path: direct D3D11 frame send");
    }
    else
    {
        bool needRebuildGraph = false;
        if (m_filterGraph)
        {
            if (m_filterSrcW != static_cast<int>(desc.Width) ||
                m_filterSrcH != static_cast<int>(desc.Height) ||
                m_filterNeedScale != false ||
                !m_filterFramesCtx || !d3d11Frame->hw_frames_ctx || m_filterFramesCtx->data != d3d11Frame->hw_frames_ctx->data)
            {
                needRebuildGraph = true;
            }
        }
        if (needRebuildGraph)
        {
            avfilter_graph_free(&m_filterGraph);
            m_filterGraph = nullptr;
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
            if (m_hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_QSV)
                m_qsvFramesBound = false;
            LOG_INFO("Rebuilding zero-copy filter graph: src {}x{}", desc.Width, desc.Height);
        }

        if (!m_filterGraph)
        {
            m_filterGraph = avfilter_graph_alloc();
            if (!m_filterGraph)
            {
                LOG_ERROR("Failed to allocate filter graph");
                av_frame_free(&d3d11Frame);
                m_zeroCopyHealthy = false;
                LOG_WARN("Disable zero-copy path after filter graph allocation failure");
                locker.unlock();
                return encodeGPU(in);
            }

            const AVFilter *bufferSrc = avfilter_get_by_name("buffer");
            char args[512];
            snprintf(args, sizeof(args),
                     "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                     desc.Width, desc.Height, AV_PIX_FMT_D3D11,
                     1, m_fps, 1, 1);

            ret = avfilter_graph_create_filter(&m_bufferSrcCtx, bufferSrc, "in",
                                               args, nullptr, m_filterGraph);
            if (ret < 0)
            {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("Failed to create buffer source filter: {}", errbuf);
                av_frame_free(&d3d11Frame);
                m_zeroCopyHealthy = false;
                LOG_WARN("Disable zero-copy path after buffer source creation failure");
                locker.unlock();
                return encodeGPU(in);
            }

            AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
            if (!par)
            {
                LOG_ERROR("Failed to allocate AVBufferSrcParameters");
                av_frame_free(&d3d11Frame);
                m_zeroCopyHealthy = false;
                LOG_WARN("Disable zero-copy path after AVBufferSrcParameters failure");
                locker.unlock();
                return encodeGPU(in);
            }
            par->hw_frames_ctx = d3d11Frame->hw_frames_ctx;
            av_buffersrc_parameters_set(m_bufferSrcCtx, par);
            av_free(par);

            AVFilterContext *hwmapCtx = nullptr;
            AVFilterContext *formatCtx = nullptr;

            const AVFilter *hwmap = avfilter_get_by_name("hwmap");
            const char *deriveDevice = nullptr;
            bool needHwmap = true;
            switch (m_hwaccelInfo->hwDeviceType)
            {
            case AIRAN_AV_HWDEVICE_TYPE_QSV:
                deriveDevice = "derive_device=qsv";
                break;
            case AIRAN_AV_HWDEVICE_TYPE_CUDA:
                deriveDevice = "derive_device=cuda";
                break;
            case AIRAN_AV_HWDEVICE_TYPE_VAAPI:
                deriveDevice = "derive_device=vaapi";
                break;
            case AIRAN_AV_HWDEVICE_TYPE_D3D11VA:
                needHwmap = false;
                break;
            case AIRAN_AV_HWDEVICE_TYPE_D3D12VA:
                deriveDevice = "derive_device=d3d12va";
                break;
            case AIRAN_AV_HWDEVICE_TYPE_VULKAN:
                deriveDevice = "derive_device=vulkan";
                break;
            default:
                needHwmap = false;
                break;
            }

            if (needHwmap)
            {
                ret = avfilter_graph_create_filter(&hwmapCtx, hwmap, "hwmap",
                                                   deriveDevice, nullptr, m_filterGraph);
                if (ret < 0)
                {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    LOG_ERROR("Failed to create hwmap filter: {}", errbuf);
                    av_frame_free(&d3d11Frame);
                    m_zeroCopyHealthy = false;
                    LOG_WARN("Disable zero-copy path after hwmap creation failure");
                    locker.unlock();
                    return encodeGPU(in);
                }
            }

            const char *pixFmts = nullptr;
            switch (m_hwaccelInfo->hwDeviceType)
            {
            case AIRAN_AV_HWDEVICE_TYPE_QSV:
                pixFmts = "pix_fmts=qsv";
                break;
            case AIRAN_AV_HWDEVICE_TYPE_CUDA:
                pixFmts = "pix_fmts=cuda";
                break;
            case AIRAN_AV_HWDEVICE_TYPE_VAAPI:
                pixFmts = "pix_fmts=vaapi";
                break;
            case AIRAN_AV_HWDEVICE_TYPE_D3D11VA:
                pixFmts = "pix_fmts=d3d11";
                break;
            case AIRAN_AV_HWDEVICE_TYPE_D3D12VA:
                pixFmts = "pix_fmts=d3d12va";
                break;
            case AIRAN_AV_HWDEVICE_TYPE_VULKAN:
                pixFmts = "pix_fmts=vulkan";
                break;
            default:
                pixFmts = nullptr;
                break;
            }
            if (pixFmts)
            {
                const AVFilter *format = avfilter_get_by_name("format");
                ret = avfilter_graph_create_filter(&formatCtx, format, "format",
                                                   pixFmts, nullptr, m_filterGraph);
                if (ret < 0)
                {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    LOG_ERROR("Failed to create format filter: {}", errbuf);
                    av_frame_free(&d3d11Frame);
                    m_zeroCopyHealthy = false;
                    LOG_WARN("Disable zero-copy path after format creation failure");
                    locker.unlock();
                    return encodeGPU(in);
                }
            }

            const char *usedScaleName = "none";

            const AVFilter *bufferSink = avfilter_get_by_name("buffersink");
            ret = avfilter_graph_create_filter(&m_bufferSinkCtx, bufferSink, "out",
                                               nullptr, nullptr, m_filterGraph);
            if (ret < 0)
            {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("Failed to create buffer sink filter: {}", errbuf);
                av_frame_free(&d3d11Frame);
                m_zeroCopyHealthy = false;
                LOG_WARN("Disable zero-copy path after buffer sink creation failure");
                locker.unlock();
                return encodeGPU(in);
            }

            AVFilterContext *prevCtx = m_bufferSrcCtx;
            if (hwmapCtx)
            {
                ret = avfilter_link(prevCtx, 0, hwmapCtx, 0);
                if (ret < 0)
                {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    LOG_ERROR("Failed to link buffer->hwmap: {}", errbuf);
                    av_frame_free(&d3d11Frame);
                    m_zeroCopyHealthy = false;
                    LOG_WARN("Disable zero-copy path after filter link failure");
                    locker.unlock();
                    return encodeGPU(in);
                }
                prevCtx = hwmapCtx;
            }
            if (formatCtx)
            {
                ret = avfilter_link(prevCtx, 0, formatCtx, 0);
                if (ret < 0)
                {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    LOG_ERROR("Failed to link to format: {}", errbuf);
                    av_frame_free(&d3d11Frame);
                    m_zeroCopyHealthy = false;
                    LOG_WARN("Disable zero-copy path after filter link failure");
                    locker.unlock();
                    return encodeGPU(in);
                }
                prevCtx = formatCtx;
            }
            ret = avfilter_link(prevCtx, 0, m_bufferSinkCtx, 0);

            if (ret < 0)
            {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("Failed to link filters: {}", errbuf);
                av_frame_free(&d3d11Frame);
                m_zeroCopyHealthy = false;
                LOG_WARN("Disable zero-copy path after filter link failure");
                locker.unlock();
                return encodeGPU(in);
            }

            ret = avfilter_graph_config(m_filterGraph, nullptr);
            if (ret < 0)
            {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("Failed to configure filter graph: {}", errbuf);
                av_frame_free(&d3d11Frame);
                m_zeroCopyHealthy = false;
                LOG_WARN("Disable zero-copy path after graph config failure");
                locker.unlock();
                return encodeGPU(in);
            }

            m_filterSrcW = static_cast<int>(desc.Width);
            m_filterSrcH = static_cast<int>(desc.Height);
            m_filterNeedScale = false;
            if (m_filterFramesCtx)
                av_buffer_unref(&m_filterFramesCtx);
            m_filterFramesCtx = av_buffer_ref(d3d11Frame->hw_frames_ctx);

            if (m_hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_QSV && !m_qsvFramesBound)
            {
#if AIRAN_FFMPEG_HAS_BUFFERSINK_HW_FRAMES_CTX
                AVBufferRef *sinkFramesCtx = av_buffersink_get_hw_frames_ctx(m_bufferSinkCtx);
                if (sinkFramesCtx)
                {
                    if (!reinitializeQsvCodecWithGraphFrames(sinkFramesCtx))
                    {
                        m_zeroCopyHealthy = false;
                        av_frame_free(&d3d11Frame);
                        locker.unlock();
                        return encodeGPU(in);
                    }
                }
                else
                {
                    LOG_WARN("QSV graph configured but buffersink has no hw_frames_ctx yet; will try after first output frame");
                }
#else
                LOG_WARN("FFmpeg build has no av_buffersink_get_hw_frames_ctx; QSV graph frame binding will use first output frame");
#endif
            }

            LOG_INFO("Initialized zero-copy graph: mode={}, hw={}, scale={}",
                     "passthrough",
                     m_hwaccelInfo->hwDeviceTypeName,
                     usedScaleName);
        }

        d3d11Frame->pts = m_pts;
        ret = av_buffersrc_add_frame_flags(m_bufferSrcCtx, d3d11Frame, 0);
        if (ret < 0)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Failed to add frame to buffer source: {}", errbuf);

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
            m_zeroCopyHealthy = false;
            LOG_WARN("Disable zero-copy path after buffersrc add_frame failure");

            av_frame_free(&d3d11Frame);
            locker.unlock();
            return encodeGPU(in);
        }

        qsvFrame = av_frame_alloc();
        if (!qsvFrame)
        {
            av_frame_free(&d3d11Frame);
            m_zeroCopyHealthy = false;
            LOG_WARN("Disable zero-copy path after frame allocation failure");
            locker.unlock();
            return encodeGPU(in);
        }

        ret = av_buffersink_get_frame(m_bufferSinkCtx, qsvFrame);
        if (ret < 0)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Failed to get frame from buffer sink: {}", errbuf);
            av_frame_free(&qsvFrame);
            av_frame_free(&d3d11Frame);
            m_zeroCopyHealthy = false;
            LOG_WARN("Disable zero-copy path after buffersink failure");
            locker.unlock();
            return encodeGPU(in);
        }

        if (m_hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_QSV &&
            !m_qsvFramesBound && qsvFrame->hw_frames_ctx &&
            (!m_codecContext->hw_frames_ctx || m_codecContext->hw_frames_ctx->data != qsvFrame->hw_frames_ctx->data))
        {
            /* 保存旧的 frames_ctx 指针用于重建尝试 */
            AVBufferRef *oldFramesCtx = av_buffer_ref(qsvFrame->hw_frames_ctx);
            /* 释放当前从 buffersink 取到的帧（属于旧的 frames_ctx） */
            av_frame_free(&qsvFrame);

            if (!reinitializeQsvCodecWithGraphFrames(oldFramesCtx))
            {
                av_buffer_unref(&oldFramesCtx);
                m_zeroCopyHealthy = false;
                av_frame_free(&d3d11Frame);
                locker.unlock();
                return encodeGPU(in);
            }

            /* reinitialize 成功，尝试把触发帧重新 push 进 filter graph 并获取来自新会话的输出帧 */
            av_buffer_unref(&oldFramesCtx);

            /* 重新 push 原始 d3d11Frame 到 buffersrc */
            d3d11Frame->pts = m_pts; /* 保持 pts */
            ret = av_buffersrc_add_frame_flags(m_bufferSrcCtx, d3d11Frame, 0);
            if (ret < 0)
            {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("After reinit: failed to re-add frame to buffer source: {}", errbuf);
                m_zeroCopyHealthy = false;
                av_frame_free(&d3d11Frame);
                locker.unlock();
                return encodeGPU(in);
            }

            /* 获取新会话下的输出帧 */
            qsvFrame = av_frame_alloc();
            if (!qsvFrame)
            {
                LOG_ERROR("After reinit: failed to allocate qsvFrame");
                m_zeroCopyHealthy = false;
                av_frame_free(&d3d11Frame);
                locker.unlock();
                return encodeGPU(in);
            }

            ret = av_buffersink_get_frame(m_bufferSinkCtx, qsvFrame);
            if (ret < 0)
            {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("After reinit: failed to get frame from buffer sink: {}", errbuf);
                av_frame_free(&qsvFrame);
                av_frame_free(&d3d11Frame);
                m_zeroCopyHealthy = false;
                LOG_WARN("Disable zero-copy path after buffersink failure (post-reinit)");
                locker.unlock();
                return encodeGPU(in);
            }
        }
    }

    AVFrame *encodeFrame = qsvFrame;
    AVFrame *compatFrame = nullptr;
    if (m_codecContext->hw_frames_ctx && qsvFrame && qsvFrame->hw_frames_ctx &&
        m_codecContext->hw_frames_ctx->data != qsvFrame->hw_frames_ctx->data)
    {
        if (m_hwaccelInfo && m_hwaccelInfo->hwDeviceType == AIRAN_AV_HWDEVICE_TYPE_D3D11VA)
        {
            LOG_WARN("zero-copy: d3d11 frame hw_frames_ctx is incompatible with encoder context, disable zero-copy and fallback");
            m_zeroCopyHealthy = false;
            if (qsvFrame != d3d11Frame)
                av_frame_free(&qsvFrame);
            av_frame_free(&d3d11Frame);
            locker.unlock();
            return encodeGPU(in);
        }

        compatFrame = av_frame_alloc();
        if (compatFrame)
        {
            int getRet = av_hwframe_get_buffer(m_codecContext->hw_frames_ctx, compatFrame, 0);
            if (getRet >= 0)
            {
                int txRet = av_hwframe_transfer_data(compatFrame, qsvFrame, 0);
                if (txRet >= 0)
                {
                    encodeFrame = compatFrame;
                    LOG_TRACE("zero-copy: transferred hw frame to encoder hw_frames_ctx for session compatibility");
                }
                else
                {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(txRet, errbuf, sizeof(errbuf));
                    LOG_WARN("zero-copy: hwframe transfer to encoder context failed: {}, use original frame", errbuf);
                    av_frame_free(&compatFrame);
                }
            }
            else
            {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(getRet, errbuf, sizeof(errbuf));
                LOG_WARN("zero-copy: alloc frame on encoder hw_frames_ctx failed: {}, use original frame", errbuf);
                av_frame_free(&compatFrame);
            }
        }
    }

    LOG_TRACE("zero-copy send: frame_fmt={}, frame_hwctx=0x{:x}, codec_hwctx=0x{:x}",
              av_get_pix_fmt_name(static_cast<AVPixelFormat>(encodeFrame->format)),
              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(encodeFrame->hw_frames_ctx ? encodeFrame->hw_frames_ctx->data : nullptr)),
              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(m_codecContext->hw_frames_ctx ? m_codecContext->hw_frames_ctx->data : nullptr)));

    prepareFrameForEncode(encodeFrame);
    ret = sendFrameToEncoder(encodeFrame, out, timestampUs);
    if (ret == AVERROR(EAGAIN))
    {
        /* 编码器内部队列满：先排空一次旧包，再重试发送当前帧 */
        appendAvailablePackets(out, timestampUs);
        ret = sendFrameToEncoder(encodeFrame, out, timestampUs);
    }

    if (compatFrame)
        av_frame_free(&compatFrame);
    if (qsvFrame != d3d11Frame)
        av_frame_free(&qsvFrame);
    av_frame_free(&d3d11Frame);

    if (ret < 0)
    {
        if (ret == AVERROR(EAGAIN))
        {
            LOG_TRACE("Encoder still back-pressured after retry in zero-copy path, skip this frame");
            return {out, timestampUs};
        }
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Error sending frame to encoder (zero-copy): {}", errbuf);
        return {out, timestampUs};
    }

    appendAvailablePackets(out, timestampUs);

    return {out, timestampUs};
}
#endif
