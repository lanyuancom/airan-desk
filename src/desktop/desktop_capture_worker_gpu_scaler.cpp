/* Split from desktop_capture_worker_gpu.cpp by GPU capture responsibility. */

#include "desktop_capture_worker.h"

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
bool DesktopCaptureWorker::scaleTextureForSubscriberLocked(ID3D11Texture2D *srcTexture, SubscriberInfo *subscriber, ID3D11Texture2D *&outTexture)
{
    outTexture = nullptr;
    if (!srcTexture || !subscriber || subscriber->dstW <= 0 || subscriber->dstH <= 0)
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTexture->GetDesc(&srcDesc);

    ID3D11Device *rawDevice = nullptr;
    srcTexture->GetDevice(&rawDevice);
    if (!rawDevice)
    {
        return false;
    }

    const bool deviceChanged = !subscriber->scaleDevice || subscriber->scaleDevice.Get() != rawDevice;
    const bool configChanged = deviceChanged || !subscriber->scaleOutputTex ||
                               subscriber->scaleSrcW != srcDesc.Width ||
                               subscriber->scaleSrcH != srcDesc.Height ||
                               subscriber->scaleSrcFormat != srcDesc.Format;

    if (deviceChanged)
    {
        subscriber->scaleVideoDevice.Reset();
        subscriber->scaleVideoContext.Reset();
        subscriber->scaleVpEnum.Reset();
        subscriber->scaleProcessor.Reset();
        subscriber->scaleInputTex.Reset();
        subscriber->scaleInputView.Reset();
        subscriber->scaleOutputTex.Reset();
        subscriber->scaleOutputView.Reset();

        subscriber->scaleDevice = rawDevice;

        ComPtr<ID3D11VideoDevice> videoDev;
        if (FAILED(rawDevice->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void **>(videoDev.GetAddressOf()))) || !videoDev)
        {
            LOG_ERROR("GPU scaler: QueryInterface(ID3D11VideoDevice) failed for subscriber {}", subscriber->id);
            rawDevice->Release();
            return false;
        }

        ComPtr<ID3D11DeviceContext> immediateCtx;
        rawDevice->GetImmediateContext(&immediateCtx);
        if (!immediateCtx)
        {
            LOG_ERROR("GPU scaler: GetImmediateContext failed for subscriber {}", subscriber->id);
            rawDevice->Release();
            return false;
        }

        ComPtr<ID3D11VideoContext> videoCtx;
        if (FAILED(immediateCtx->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void **>(videoCtx.GetAddressOf()))) || !videoCtx)
        {
            LOG_ERROR("GPU scaler: QueryInterface(ID3D11VideoContext) failed for subscriber {}", subscriber->id);
            rawDevice->Release();
            return false;
        }

        subscriber->scaleVideoDevice = videoDev;
        subscriber->scaleVideoContext = videoCtx;
    }

    rawDevice->Release();

    if (!subscriber->scaleVideoDevice || !subscriber->scaleVideoContext)
    {
        return false;
    }

    if (configChanged)
    {
        subscriber->scaleVpEnum.Reset();
        subscriber->scaleProcessor.Reset();
        subscriber->scaleInputTex.Reset();
        subscriber->scaleInputView.Reset();
        subscriber->scaleOutputTex.Reset();
        subscriber->scaleOutputView.Reset();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        contentDesc.InputWidth = srcDesc.Width;
        contentDesc.InputHeight = srcDesc.Height;
        contentDesc.OutputWidth = static_cast<UINT>(subscriber->dstW);
        contentDesc.OutputHeight = static_cast<UINT>(subscriber->dstH);
        contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        HRESULT hr = subscriber->scaleVideoDevice->CreateVideoProcessorEnumerator(&contentDesc, &subscriber->scaleVpEnum);
        if (FAILED(hr) || !subscriber->scaleVpEnum)
        {
            LOG_ERROR("GPU scaler: CreateVideoProcessorEnumerator failed for subscriber {} hr={:#x}",
                      subscriber->id,
                      static_cast<unsigned int>(hr));
            return false;
        }

        hr = subscriber->scaleVideoDevice->CreateVideoProcessor(subscriber->scaleVpEnum.Get(), 0, &subscriber->scaleProcessor);
        if (FAILED(hr) || !subscriber->scaleProcessor)
        {
            LOG_ERROR("GPU scaler: CreateVideoProcessor failed for subscriber {} hr={:#x}",
                      subscriber->id,
                      static_cast<unsigned int>(hr));
            return false;
        }

        UINT fmtFlags = 0;
        hr = subscriber->scaleVpEnum->CheckVideoProcessorFormat(
            srcDesc.Format,
            &fmtFlags);
        if (FAILED(hr) || ((fmtFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0))
        {
            LOG_ERROR("GPU scaler: source format {} is not VP-input compatible for subscriber {} hr={:#x} flags={:#x}",
                      static_cast<int>(srcDesc.Format),
                      subscriber->id,
                      static_cast<unsigned int>(hr),
                      static_cast<unsigned int>(fmtFlags));
            return false;
        }

        D3D11_TEXTURE2D_DESC inDesc{};
        inDesc.Width = srcDesc.Width;
        inDesc.Height = srcDesc.Height;
        inDesc.MipLevels = 1;
        inDesc.ArraySize = 1;
        inDesc.Format = srcDesc.Format;
        inDesc.SampleDesc.Count = 1;
        inDesc.Usage = D3D11_USAGE_DEFAULT;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
        inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputViewDesc.Texture2D.MipSlice = 0;
        inputViewDesc.Texture2D.ArraySlice = 0;

        const UINT bindCandidates[] = {
            D3D11_BIND_DECODER,
            D3D11_BIND_SHADER_RESOURCE,
            0u};

        bool inputReady = false;
        HRESULT lastCreateTexHr = S_OK;
        HRESULT lastCreateViewHr = S_OK;
        UINT chosenBindFlags = 0;

        for (UINT bindFlags : bindCandidates)
        {
            subscriber->scaleInputTex.Reset();
            subscriber->scaleInputView.Reset();

            inDesc.BindFlags = bindFlags;
            HRESULT texHr = subscriber->scaleDevice->CreateTexture2D(&inDesc, nullptr, &subscriber->scaleInputTex);
            if (FAILED(texHr) || !subscriber->scaleInputTex)
            {
                lastCreateTexHr = texHr;
                continue;
            }

            HRESULT viewHr = subscriber->scaleVideoDevice->CreateVideoProcessorInputView(
                subscriber->scaleInputTex.Get(),
                subscriber->scaleVpEnum.Get(),
                &inputViewDesc,
                &subscriber->scaleInputView);
            if (FAILED(viewHr) || !subscriber->scaleInputView)
            {
                lastCreateViewHr = viewHr;
                continue;
            }

            chosenBindFlags = bindFlags;
            inputReady = true;
            break;
        }

        if (!inputReady)
        {
            LOG_ERROR("GPU scaler: failed to prepare input texture/view for subscriber {} (fmt={}, lastCreateTexHr={:#x}, lastCreateViewHr={:#x})",
                      subscriber->id,
                      static_cast<int>(srcDesc.Format),
                      static_cast<unsigned int>(lastCreateTexHr),
                      static_cast<unsigned int>(lastCreateViewHr));
            return false;
        }

        LOG_INFO("GPU scaler: input texture/view ready for subscriber {} with bindFlags={:#x}",
                 subscriber->id,
                 static_cast<unsigned int>(chosenBindFlags));

        D3D11_TEXTURE2D_DESC outDesc{};
        outDesc.Width = static_cast<UINT>(subscriber->dstW);
        outDesc.Height = static_cast<UINT>(subscriber->dstH);
        outDesc.MipLevels = 1;
        outDesc.ArraySize = 1;
        outDesc.Format = srcDesc.Format;
        outDesc.SampleDesc.Count = 1;
        outDesc.Usage = D3D11_USAGE_DEFAULT;
        outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        hr = subscriber->scaleDevice->CreateTexture2D(&outDesc, nullptr, &subscriber->scaleOutputTex);
        if (FAILED(hr) || !subscriber->scaleOutputTex)
        {
            LOG_ERROR("GPU scaler: CreateTexture2D(output) failed for subscriber {} hr={:#x}",
                      subscriber->id,
                      static_cast<unsigned int>(hr));
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outViewDesc{};
        outViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outViewDesc.Texture2D.MipSlice = 0;
        hr = subscriber->scaleVideoDevice->CreateVideoProcessorOutputView(
            subscriber->scaleOutputTex.Get(),
            subscriber->scaleVpEnum.Get(),
            &outViewDesc,
            &subscriber->scaleOutputView);
        if (FAILED(hr) || !subscriber->scaleOutputView)
        {
            LOG_ERROR("GPU scaler: CreateVideoProcessorOutputView failed for subscriber {} hr={:#x}",
                      subscriber->id,
                      static_cast<unsigned int>(hr));
            return false;
        }

        subscriber->scaleSrcW = srcDesc.Width;
        subscriber->scaleSrcH = srcDesc.Height;
        subscriber->scaleSrcFormat = srcDesc.Format;
        LOG_INFO("Configured GPU scaler for subscriber {}: {}x{} -> {}x{}",
                 subscriber->id,
                 srcDesc.Width,
                 srcDesc.Height,
                 subscriber->dstW,
                 subscriber->dstH);
    }

    if (!subscriber->scaleInputTex || !subscriber->scaleInputView)
    {
        LOG_ERROR("GPU scaler: scaler input resources not ready for subscriber {}", subscriber->id);
        return false;
    }

    ComPtr<ID3D11DeviceContext> immediateCtx;
    subscriber->scaleDevice->GetImmediateContext(&immediateCtx);
    if (!immediateCtx)
    {
        LOG_ERROR("GPU scaler: GetImmediateContext before CopyResource failed for subscriber {}", subscriber->id);
        return false;
    }

    immediateCtx->CopyResource(subscriber->scaleInputTex.Get(), srcTexture);

    RECT srcRect{0, 0, static_cast<LONG>(srcDesc.Width), static_cast<LONG>(srcDesc.Height)};
    RECT dstRect{0, 0, subscriber->dstW, subscriber->dstH};

    subscriber->scaleVideoContext->VideoProcessorSetStreamFrameFormat(
        subscriber->scaleProcessor.Get(),
        0,
        D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    subscriber->scaleVideoContext->VideoProcessorSetStreamSourceRect(
        subscriber->scaleProcessor.Get(),
        0,
        TRUE,
        &srcRect);
    subscriber->scaleVideoContext->VideoProcessorSetStreamDestRect(
        subscriber->scaleProcessor.Get(),
        0,
        TRUE,
        &dstRect);
    subscriber->scaleVideoContext->VideoProcessorSetOutputTargetRect(
        subscriber->scaleProcessor.Get(),
        TRUE,
        &dstRect);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = subscriber->scaleInputView.Get();

    HRESULT hr = subscriber->scaleVideoContext->VideoProcessorBlt(
        subscriber->scaleProcessor.Get(),
        subscriber->scaleOutputView.Get(),
        0,
        1,
        &stream);
    if (FAILED(hr))
    {
        LOG_ERROR("GPU scaler: VideoProcessorBlt failed for subscriber {} hr={:#x}",
                  subscriber->id,
                  static_cast<unsigned int>(hr));
        return false;
    }

    outTexture = subscriber->scaleOutputTex.Get();
    outTexture->AddRef();
    return true;
}
#endif
