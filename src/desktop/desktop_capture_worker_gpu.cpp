/* Split from desktop_capture_worker_gpu.cpp by GPU capture responsibility. */

#include "desktop_capture_worker.h"
#include "desktop_grab.h"
#include "../codec/h264_encoder.h"

#include <QDateTime>
#include <algorithm>

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
static bool copyGpuTextureToQImage(ID3D11Texture2D *srcTexture, QImage &outImage)
{
    if (!srcTexture)
        return false;

    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTexture->GetDesc(&srcDesc);
    if (srcDesc.Width == 0 || srcDesc.Height == 0)
        return false;

    ID3D11Device *device = nullptr;
    srcTexture->GetDevice(&device);
    if (!device)
        return false;

    ID3D11DeviceContext *ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx)
    {
        device->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = srcDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.ArraySize = 1;
    stagingDesc.MipLevels = 1;
    stagingDesc.SampleDesc.Count = 1;

    ID3D11Texture2D *staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr) || !staging)
    {
        ctx->Release();
        device->Release();
        return false;
    }

    ctx->CopyResource(staging, srcTexture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        staging->Release();
        ctx->Release();
        device->Release();
        return false;
    }

    QImage image(static_cast<int>(srcDesc.Width), static_cast<int>(srcDesc.Height), QImage::Format_ARGB32);
    if (image.isNull())
    {
        ctx->Unmap(staging, 0);
        staging->Release();
        ctx->Release();
        device->Release();
        return false;
    }

    const int srcStride = static_cast<int>(mapped.RowPitch);
    const int dstStride = image.bytesPerLine();
    const int copyBytes = std::min(srcStride, dstStride);
    for (UINT y = 0; y < srcDesc.Height; ++y)
    {
        memcpy(image.bits() + y * dstStride,
               static_cast<uint8_t *>(mapped.pData) + y * srcStride,
               copyBytes);
    }

    ctx->Unmap(staging, 0);
    staging->Release();
    ctx->Release();
    device->Release();

    outImage = image;
    return true;
}
#endif

bool DesktopCaptureWorker::captureAndEncodeGPU()
{
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    QMutexLocker locker(&m_mutex);
    ID3D11Texture2D *texture = nullptr;
    if (!m_desktopGrab)
    {
        return false;
    }
    if (m_actualCaptureBackend == QStringLiteral("qt"))
    {
        return false;
    }

    const bool grabOk = m_desktopGrab->grabFrameGPU(texture);
    if (!grabOk)
    {
        if (!m_gpuLogTimer.isValid())
        {
            m_gpuLogTimer.start();
        }
        const qint64 nowMs = m_gpuLogTimer.elapsed();
        if (nowMs - m_lastGpuGrabWarnMs > 1000)
        {
            LOG_WARN("Failed to grab GPU frame");
            m_lastGpuGrabWarnMs = nowMs;
        }
        return false;
    }

    LOG_TRACE("grabFrameGPU returned grabOk={}, texture={}", grabOk, (void *)texture);

    if (!texture)
    {
        /* GPU path reports success but no updated frame (DXGI timeout): normal, no fallback needed. */
        LOG_TRACE("GPU grab succeeded but texture is null (no new frame)");
        if (anySubscriberNeedsStaticCpuRefreshLocked(QDateTime::currentMSecsSinceEpoch()))
            return false;
        return true;
    }
    LOG_TRACE("GPU texture obtained, subscribers to encode: {}", m_subscribers.size());
    D3D11_TEXTURE2D_DESC srcDesc{};
    texture->GetDesc(&srcDesc);
    /* 遍历所有订阅者，使用各自的编码器编码 */
    bool encodedAnyFrame = false;
    for (auto it : m_subscribers.values())
    {
        ID3D11Texture2D *encodeTexture = texture;
        ID3D11Texture2D *scaledTexture = nullptr;

        if (it->dstW > 0 && it->dstH > 0 &&
            (static_cast<UINT>(it->dstW) != srcDesc.Width || static_cast<UINT>(it->dstH) != srcDesc.Height))
        {
            if (!scaleTextureForSubscriberLocked(texture, it, scaledTexture))
            {
                LOG_TRACE("GPU scaling failed for subscriber {} ({}x{} -> {}x{}), fallback to encoder path",
                          it->id,
                          srcDesc.Width,
                          srcDesc.Height,
                          it->dstW,
                          it->dstH);

                /* 兜底：GPU缩放失败时，转CPU缩放避免黑屏（仅失败路径触发） */
                QImage cpuImage;
                if (copyGpuTextureToQImage(texture, cpuImage) && !cpuImage.isNull())
                {
                    QImage scaledImage = cpuImage.scaled(it->dstW, it->dstH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    auto cpuResult = it->encoder->encodeCPU(scaledImage);
                    if (!cpuResult.first->empty())
                    {
                        LOG_TRACE("Encoded CPU-fallback frame for subscriber {} -> {} bytes", it->id, cpuResult.first->size());
                        emit frameEncoded(it->id, cpuResult.first, cpuResult.second);
                        it->lastEncodedMs = QDateTime::currentMSecsSinceEpoch();
                        it->forceNextFrame = false;
                        encodedAnyFrame = true;
                        continue;
                    }
                    LOG_TRACE("CPU-fallback encode returned empty for subscriber {}", it->id);
                }
                else
                {
                    LOG_TRACE("CPU-fallback texture copy failed for subscriber {}", it->id);
                }

                /* 关键修复：GPU缩放失败且CPU兜底也失败时，不能再把原尺寸纹理送入编码器。 */
                /* 否则会每帧触发 size mismatch（例如 1317x823 -> 1312x816）。 */
                continue;
            }
            else
            {
                encodeTexture = scaledTexture;
            }
        }

        if (it->encoder)
        {
            std::pair<std::shared_ptr<rtc::binary>, quint64> result{};
            if (it->encoder->supportsZeroCopyEncoder())
            {
                result = it->encoder->zeroCopyEncodeGPU(encodeTexture);
            }

            if (result.first == nullptr || result.first->empty())
            {
                result = it->encoder->encodeGPU(encodeTexture);
            }
            const bool zeroCopyActive = isZeroCopyActiveLocked(it->encoder.get());
            if (zeroCopyActive != it->zeroCopyActive)
            {
                it->zeroCopyActive = zeroCopyActive;
                emit encoderChanged(m_screenIndex,
                                    it->encoder->encoderName(),
                                    it->encoder->isHardwareEncoder(),
                                    it->zeroCopyActive);
            }
            if (result.first != nullptr && !result.first->empty())
            {
                LOG_TRACE("Encoded GPU frame for subscriber {} -> {} bytes", it->id, result.first->size());
                emit frameEncoded(it->id, result.first, result.second);
                it->lastEncodedMs = QDateTime::currentMSecsSinceEpoch();
                it->lastDirtyMs = it->lastEncodedMs;
                it->forceNextFrame = false;
                encodedAnyFrame = true;
            }
            else
            {
                LOG_TRACE("encodeGPU returned empty for subscriber {}", it->id);
            }
        }

        if (scaledTexture)
        {
            scaledTexture->Release();
            scaledTexture = nullptr;
        }
    }
    if (m_desktopGrab && texture)
    {
        m_desktopGrab->releaseLastFrame(texture);
    }

    /* 本轮GPU路径无任何有效输出时，返回false让调用方回退到CPU路径，避免出现“持续无画面”。 */
    if (!m_subscribers.isEmpty() && !encodedAnyFrame)
    {
        LOG_WARN("GPU path produced no encoded frame in this tick, fallback to CPU path");
        return false;
    }

    return true;
#endif
    return false;
}
