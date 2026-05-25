/* Split from h264_decoder.cpp by decoder responsibility. */

#include "h264_decoder.h"

#include <algorithm>
#include <vector>

namespace
{
    void appendTransferCandidate(std::vector<AVPixelFormat> &candidates, AVPixelFormat fmt)
    {
        if (fmt == AV_PIX_FMT_NONE)
        {
            return;
        }
        if (std::find(candidates.begin(), candidates.end(), fmt) == candidates.end())
        {
            candidates.push_back(fmt);
        }
    }
}

QImage H264Decoder::decodeFrame(const rtc::binary &h264Data)
{
    QMutexLocker locker(&m_mutex);
    m_lastDecodeHadError = false;

    if (!m_initialized)
    {
        LOG_ERROR("Decoder not initialized");
        m_lastDecodeHadError = true;
        return QImage();
    }
    if (h264Data.empty())
    {
        return QImage();
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet)
    {
        LOG_ERROR("Failed to allocate AVPacket");
        m_lastDecodeHadError = true;
        return QImage();
    }

    /* 关键：不要直接让 packet->data 指向 rtc::binary 的内存 */
    /* 在部分平台 FFmpeg 版本/优化下会触发未定义行为（尤其是 ARM 上更容易 SIGBUS） */
    /* 这里改为创建引用计数 packet buffer 并拷贝数据 */
    int pktSize = static_cast<int>(h264Data.size());
    int ret = av_new_packet(packet, pktSize);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("av_new_packet failed: {}", errbuf);
        av_packet_free(&packet);
        m_lastDecodeHadError = true;
        return QImage();
    }
    memcpy(packet->data, h264Data.data(), pktSize);

    ret = sendPacketToDecoder(packet);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Error sending packet to decoder: {}", errbuf);
        av_packet_free(&packet);
        m_lastDecodeHadError = true;
        if (m_codecInfo && m_codecInfo->isHardware && m_hwAccelInfo)
        {
            const AVHWDeviceType failedHwType = m_hwAccelInfo->hwDeviceType;
            const QString failedHwName = m_hwAccelInfo->hwDeviceTypeName;
            m_disabledHwTypes.insert(static_cast<int>(failedHwType));
            LOG_WARN("Blacklisted failed hw backend {} after packet send failure, reinitializing decoder",
                     failedHwName);

            cleanup();
            locker.unlock();
            const bool reinitOk = initialize();
            locker.relock();
            if (!reinitOk)
            {
                LOG_ERROR("Decoder reinitialize failed after blacklisting {}", failedHwName);
            }
        }
        return QImage();
    }

    QImage result;
    bool gotFrame = false;
#if !AIRAN_FFMPEG_HAS_SEND_RECEIVE_API
    AVFrame *legacyFrame = av_frame_alloc();
    int gotLegacyFrame = 0;
    ret = avcodec_decode_video2(m_codecContext, legacyFrame, &gotLegacyFrame, packet);
    av_packet_free(&packet);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Error decoding packet: {}", errbuf);
        av_frame_free(&legacyFrame);
        m_lastDecodeHadError = true;
        return QImage();
    }
    if (!gotLegacyFrame)
    {
        av_frame_free(&legacyFrame);
        return QImage();
    }
    gotFrame = true;
    {
        AVFrame *frameToConvert = legacyFrame;
#else
    av_packet_free(&packet);
    /* 支持多帧输出（只返回首帧，后续可扩展为信号发送全部帧） */
    while (true)
    {
        AVFrame *frameToConvert = av_frame_alloc();
        ret = receiveFrameFromDecoder(frameToConvert);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            av_frame_free(&frameToConvert);
            break;
        }
        else if (ret < 0)
        {
            av_frame_free(&frameToConvert);
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Error receiving frame from decoder: {}", errbuf);
            m_lastDecodeHadError = true;
            break;
        }
        gotFrame = true;
#endif

        bool frameHasDecodeError = false;
#ifdef AV_FRAME_FLAG_CORRUPT
        frameHasDecodeError = frameHasDecodeError || ((frameToConvert->flags & AV_FRAME_FLAG_CORRUPT) != 0);
#endif
#if defined(FF_DECODE_ERROR_INVALID_BITSTREAM) || defined(FF_DECODE_ERROR_MISSING_REFERENCE)
        frameHasDecodeError = frameHasDecodeError || (frameToConvert->decode_error_flags != 0);
#endif
        if (frameHasDecodeError)
        {
            LOG_WARN("Drop H264 frame marked corrupt by decoder, flags={}, decode_error_flags={}",
                     frameToConvert->flags, frameToConvert->decode_error_flags);
            av_frame_free(&frameToConvert);
            m_lastDecodeHadError = true;
            break;
        }

        if (m_codecInfo->isHardware)
        {
            AVPixelFormat frameFormat = static_cast<AVPixelFormat>(frameToConvert->format);
            /* 简化硬件帧判断逻辑 */
            bool isHardwareFrame = m_codecInfo->isHardware && frameToConvert->hw_frames_ctx;
            if (isHardwareFrame)
            {
                std::vector<AVPixelFormat> transferCandidates;
                /* 优先尝试 codec context 推荐的 sw_pix_fmt，通常是最兼容且性能较好的格式 */
                appendTransferCandidate(transferCandidates, m_codecContext->sw_pix_fmt);

                /* 查询 hwframe 实际可 transfer 的系统内存格式 */
                AVPixelFormat *transferFormats = nullptr;
                int fmtRet = av_hwframe_transfer_get_formats(frameToConvert->hw_frames_ctx,
                                                             AV_HWFRAME_TRANSFER_DIRECTION_FROM,
                                                             &transferFormats,
                                                             0);
                if (fmtRet >= 0 && transferFormats)
                {
                    for (int idx = 0; transferFormats[idx] != AV_PIX_FMT_NONE; ++idx)
                    {
                        appendTransferCandidate(transferCandidates, transferFormats[idx]);
                    }
                }
                if (transferFormats)
                {
                    av_freep(&transferFormats);
                }

                /* 通用兜底顺序 */
                appendTransferCandidate(transferCandidates, AV_PIX_FMT_NV12);
                appendTransferCandidate(transferCandidates, AV_PIX_FMT_YUV420P);
#ifdef AV_PIX_FMT_DRM_PRIME
                if (frameFormat == AV_PIX_FMT_DRM_PRIME)
                {
                    appendTransferCandidate(transferCandidates, AV_PIX_FMT_YUV420P);
                }
#endif

                AVFrame *convertedFrame = nullptr;
                int transferRet = AVERROR(EINVAL);
                AVPixelFormat usedFmt = AV_PIX_FMT_NONE;
                for (AVPixelFormat transferTarget : transferCandidates)
                {
                    AVFrame *swFrame = av_frame_alloc();
                    if (!swFrame)
                    {
                        continue;
                    }
                    swFrame->format = transferTarget;
                    swFrame->width = frameToConvert->width;
                    swFrame->height = frameToConvert->height;

                    int allocRet = av_frame_get_buffer(swFrame, 32);
                    if (allocRet < 0)
                    {
                        av_frame_free(&swFrame);
                        continue;
                    }

                    transferRet = av_hwframe_transfer_data(swFrame, frameToConvert, 0);
                    if (transferRet >= 0)
                    {
                        convertedFrame = swFrame;
                        usedFmt = transferTarget;
                        break;
                    }
                    av_frame_free(&swFrame);
                }

                if (!convertedFrame)
                {
                    const bool canFallbackHw = (m_codecInfo && m_codecInfo->isHardware && m_hwAccelInfo);
                    AVHWDeviceType failedHwType = AIRAN_AV_HWDEVICE_TYPE_NONE;
                    QString failedHwName;
                    if (canFallbackHw)
                    {
                        failedHwType = m_hwAccelInfo->hwDeviceType;
                        failedHwName = m_hwAccelInfo->hwDeviceTypeName;
                    }

                    av_frame_free(&frameToConvert);
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(transferRet, errbuf, sizeof(errbuf));
                    LOG_ERROR("Error transferring frame data from hardware after trying {} formats: {}",
                              transferCandidates.size(), errbuf);
                    m_lastDecodeHadError = true;

                    if (canFallbackHw && failedHwType != AIRAN_AV_HWDEVICE_TYPE_NONE)
                    {
                        m_disabledHwTypes.insert(static_cast<int>(failedHwType));
                        LOG_WARN("Blacklisted failed hw backend {}, reinitializing decoder to try next hardware backend",
                                 failedHwName);

                        cleanup();
                        locker.unlock();
                        const bool reinitOk = initialize();
                        locker.relock();
                        if (!reinitOk)
                        {
                            LOG_ERROR("Decoder reinitialize failed after blacklisting {}", failedHwName);
                        }
                    }
                    break;
                }

                av_frame_free(&frameToConvert);
                frameToConvert = convertedFrame;
                LOG_TRACE("Hardware frame transfer succeeded with sw format {}", av_get_pix_fmt_name(usedFmt));
            }
            else if (frameFormat != m_codecContext->sw_pix_fmt)
            {
                /* 软件帧统一转m_codecContext->sw_pix_fmt，简化后续处理逻辑 */
                AVFrame *tmpFrame = av_frame_alloc();
                if (convertToTargetFmt(frameToConvert, tmpFrame))
                {
                    av_frame_free(&frameToConvert);
                    frameToConvert = tmpFrame;
                }
                else
                {
                    av_frame_free(&tmpFrame);
                }
            }
        }
        if (frameToConvert)
        {
            QImage img = avframeToQImage(frameToConvert);
            av_frame_free(&frameToConvert);
            if (!img.isNull())
            {
                result = std::move(img);
            }
        }
#if AIRAN_FFMPEG_HAS_SEND_RECEIVE_API
        /* drain any additional queued frames so latency doesn't accumulate;
         * keep only the most recent rendered image. */
        continue;
#else
        break;
#endif
    }
    Q_UNUSED(gotFrame);
    return result;
}
