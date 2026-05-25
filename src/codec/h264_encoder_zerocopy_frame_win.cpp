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

} /* namespace */

static void enableD3D11MultithreadProtection(ID3D11Device *device)
{
    if (!device)
        return;

    ID3D10Multithread *multithread = nullptr;
    HRESULT hr = device->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void **>(&multithread));
    if (SUCCEEDED(hr) && multithread)
    {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }
}

/* 零拷贝：从 ID3D11Texture2D 创建 D3D11VA 硬件帧 */
AVFrame *H264Encoder::createHwFrameFromD3D11Texture(ID3D11Texture2D *texture)
{
    if (!texture)
        return nullptr;

    ID3D11Device *texDevice = nullptr;
    texture->GetDevice(&texDevice);
    if (!texDevice)
    {
        LOG_ERROR("Failed to get D3D11 device from input texture");
        return nullptr;
    }

    ID3D11DeviceContext *texImmediateCtx = nullptr;
    texDevice->GetImmediateContext(&texImmediateCtx);
    if (!texImmediateCtx)
    {
        texDevice->Release();
        LOG_ERROR("Failed to get immediate context from texture device");
        return nullptr;
    }

    enableD3D11MultithreadProtection(texDevice);

    /* 确保 D3D11VA 设备上下文存在，且与当前纹理 device 同源 */
    bool needRecreateDeviceCtx = (m_d3d11vaDeviceCtx == nullptr);
    if (!needRecreateDeviceCtx)
    {
        AVHWDeviceContext *devCtx = reinterpret_cast<AVHWDeviceContext *>(m_d3d11vaDeviceCtx->data);
        AVD3D11VADeviceContext *d3d11Ctx = devCtx ? reinterpret_cast<AVD3D11VADeviceContext *>(devCtx->hwctx) : nullptr;
        ID3D11Device *boundDev = d3d11Ctx ? d3d11Ctx->device : nullptr;
        LOG_TRACE("Zero-copy device check: texDevice=0x{:x}, boundDevice=0x{:x}",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(texDevice)),
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(boundDev)));
        if (!boundDev || boundDev != texDevice)
        {
            needRecreateDeviceCtx = true;
        }
    }

    if (needRecreateDeviceCtx)
    {
        if (m_d3d11vaFramesCtx)
        {
            av_buffer_unref(&m_d3d11vaFramesCtx);
            m_d3d11vaFramesCtx = nullptr;
        }
        if (m_d3d11vaDeviceCtx)
        {
            av_buffer_unref(&m_d3d11vaDeviceCtx);
            m_d3d11vaDeviceCtx = nullptr;
        }

        AVBufferRef *devRef = av_hwdevice_ctx_alloc(AIRAN_AV_HWDEVICE_TYPE_D3D11VA);
        if (!devRef)
        {
            texImmediateCtx->Release();
            texDevice->Release();
            LOG_ERROR("Failed to allocate D3D11VA hwdevice context");
            return nullptr;
        }

        AVHWDeviceContext *devCtx = reinterpret_cast<AVHWDeviceContext *>(devRef->data);
        AVD3D11VADeviceContext *d3d11Ctx = reinterpret_cast<AVD3D11VADeviceContext *>(devCtx->hwctx);
        d3d11Ctx->device = texDevice;
        d3d11Ctx->device_context = texImmediateCtx;
        d3d11Ctx->device->AddRef();
        d3d11Ctx->device_context->AddRef();

        int ret = av_hwdevice_ctx_init(devRef);
        if (ret < 0)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Failed to init D3D11VA device context from texture device: {}", errbuf);
            if (ret == AVERROR(ENOSYS))
            {
                LOG_WARN("Disable zero-copy for this encoder session because D3D11VA device wrapping is not implemented by this FFmpeg build");
                m_zeroCopyHealthy = false;
            }
            av_buffer_unref(&devRef);
            texImmediateCtx->Release();
            texDevice->Release();
            return nullptr;
        }

        m_d3d11vaDeviceCtx = devRef;
        LOG_INFO("Bound D3D11VA device context to desktop capture texture device: texDevice=0x{:x}",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(texDevice)));
    }

    /* 获取纹理描述 */
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    /* 复用（或按分辨率重建）D3D11VA frames ctx，避免每帧分配造成内存上涨 */
    bool needRecreateFramesCtx = (m_d3d11vaFramesCtx == nullptr);
    if (!needRecreateFramesCtx)
    {
        AVHWFramesContext *existing = reinterpret_cast<AVHWFramesContext *>(m_d3d11vaFramesCtx->data);
        if (!existing || existing->width != static_cast<int>(desc.Width) || existing->height != static_cast<int>(desc.Height))
        {
            needRecreateFramesCtx = true;
        }
    }

    if (needRecreateFramesCtx)
    {
        if (m_d3d11vaFramesCtx)
        {
            av_buffer_unref(&m_d3d11vaFramesCtx);
            m_d3d11vaFramesCtx = nullptr;
        }

        AVBufferRef *framesRef = av_hwframe_ctx_alloc(m_d3d11vaDeviceCtx);
        if (!framesRef)
        {
            LOG_ERROR("Failed to allocate reusable hwframe context for D3D11VA");
            texImmediateCtx->Release();
            texDevice->Release();
            return nullptr;
        }

        AVHWFramesContext *framesCtx = reinterpret_cast<AVHWFramesContext *>(framesRef->data);
        framesCtx->format = AV_PIX_FMT_D3D11;
        framesCtx->sw_format = AV_PIX_FMT_BGRA;
        framesCtx->width = desc.Width;
        framesCtx->height = desc.Height;
        framesCtx->initial_pool_size = 8;

        int ret = av_hwframe_ctx_init(framesRef);
        if (ret < 0)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Failed to init reusable D3D11VA hwframe context: {}", errbuf);
            if (ret == AVERROR(ENOSYS))
            {
                LOG_WARN("Disable zero-copy permanently because reusable D3D11VA hwframes are not implemented by this FFmpeg build");
                m_zeroCopyHealthy = false;
                s_zeroCopyDisabledGlobally = true;
            }
            av_buffer_unref(&framesRef);
            texImmediateCtx->Release();
            texDevice->Release();
            return nullptr;
        }
        m_d3d11vaFramesCtx = framesRef;
        LOG_INFO("Rebuilt reusable D3D11VA frames ctx: {}x{}", desc.Width, desc.Height);
    }

    /* 从绑定输入纹理的 hwframes context 中申请帧，确保 frame 属于该 context */
    AVFrame *frame = av_frame_alloc();
    if (!frame)
    {
        texImmediateCtx->Release();
        texDevice->Release();
        return nullptr;
    }

    int getBufRet = av_hwframe_get_buffer(m_d3d11vaFramesCtx, frame, 0);
    if (getBufRet < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(getBufRet, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to get D3D11 frame from reusable hwframe context: {}", errbuf);
        av_frame_free(&frame);
        texImmediateCtx->Release();
        texDevice->Release();
        return nullptr;
    }

    ID3D11Texture2D *dstTex = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
    int dstIndex = static_cast<int>(reinterpret_cast<intptr_t>(frame->data[1]));
    if (!dstTex)
    {
        texImmediateCtx->Release();
        texDevice->Release();
        av_frame_free(&frame);
        LOG_ERROR("Failed to get destination D3D11 texture from pooled hwframe");
        return nullptr;
    }

    UINT dstSubresource = D3D11CalcSubresource(0, static_cast<UINT>(dstIndex), 1);
    texImmediateCtx->CopySubresourceRegion(reinterpret_cast<ID3D11Resource *>(dstTex), dstSubresource,
                                           0, 0, 0,
                                           reinterpret_cast<ID3D11Resource *>(texture), 0, nullptr);

    texImmediateCtx->Release();
    texDevice->Release();

    return frame;
}


#endif
