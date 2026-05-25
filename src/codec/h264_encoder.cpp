#include "h264_encoder.h"
#include "hardware_context_manager.h"
#include <QtGlobal>
#include <QGuiApplication>
#include <QScreen>
#include <QStringList>
#include <cstdio>
#include <QTimer>
#include <QThread>
#include <QPointer>
#include <QFile>
#include <cstdlib>
#include <algorithm>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#include <d3d11.h>
#endif

#ifndef AV_PIX_FMT_D3D11
#define AV_PIX_FMT_D3D11 AV_PIX_FMT_D3D11VA_VLD
#endif

bool H264Encoder::s_zeroCopyDisabledGlobally = false;

H264Encoder::H264Encoder(QObject *parent)
    : QObject(parent), m_codecContext(nullptr),
      m_codecInfo(nullptr), m_frame(nullptr),
      m_hwFrame(nullptr), m_packet(nullptr),
      m_swsContext(nullptr), m_hwDeviceCtx(nullptr),
      m_dstW(0), m_dstH(0), m_fps(30), m_pts(0), m_bitrate(6000 * 1000),
      m_forceAllKeyframes(false), m_initialized(false), m_forceNextKeyframe(false), m_deviceCtx(nullptr),
      m_srcPixFmt(AV_PIX_FMT_NONE), m_cachedSwsDstFmt(AV_PIX_FMT_NONE),
      m_cachedSwsSrcW(0), m_cachedSwsSrcH(0), m_cachedSwsDstW(0), m_cachedSwsDstH(0),
      m_d3d11vaDeviceCtx(nullptr), m_d3d11vaFramesCtx(nullptr), m_filterGraph(nullptr),
      m_bufferSrcCtx(nullptr), m_bufferSinkCtx(nullptr), m_filterFramesCtx(nullptr),
      m_filterSrcW(0), m_filterSrcH(0), m_filterNeedScale(false), m_zeroCopyHealthy(true),
      m_qsvDeriveChecked(false), m_qsvDeriveOk(false),
      m_forcedQsvDeviceCtx(nullptr), m_forcedQsvFramesCtx(nullptr),
      m_forcedD3d11DeviceCtx(nullptr), m_forcedD3d11FramesCtx(nullptr),
      m_qsvSessionBound(false), m_qsvFramesBound(false), m_d3d11SessionBound(false)
{
    m_targetSwFmt = AV_PIX_FMT_NV12; /* 默认 */
}

H264Encoder::~H264Encoder()
{
    QMutexLocker locker(&m_mutex);
    cleanup();
}

bool H264Encoder::initialize(int screen_index, int dstW, int dstH, int fps, int targetKbps, bool forceAllKeyframes)
{
    QMutexLocker locker(&m_mutex);
    m_pts = 0;
    m_forceNextKeyframe = false;
    m_zeroCopyHealthy = !s_zeroCopyDisabledGlobally;
    m_qsvDeriveChecked = false;
    m_qsvDeriveOk = false;
    m_qsvSessionBound = false;
    m_qsvFramesBound = false;
    m_d3d11SessionBound = false;
    const bool bitrateMatches = targetKbps <= 0 || m_bitrate == targetKbps * 1000;
    if (m_initialized && m_dstW == dstW && m_dstH == dstH && m_fps == fps &&
        bitrateMatches && m_forceAllKeyframes == forceAllKeyframes)
    {
        LOG_INFO("Encoder already initialized with the same parameters");
        return true;
    }
    else if (m_initialized)
    {
        LOG_INFO("Encoder already initialized, resetting with new parameters");
        cleanup();
    }

    m_fps = fps;
    m_forceAllKeyframes = forceAllKeyframes;
    if (screen_index < 0)
    {
        screen_index = 0; /* 默认使用主屏幕 */
    }
    QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.isEmpty())
    {
        LOG_ERROR("No screens found for encoding");
        return false;
    }
    m_screenIndex = screen_index % screens.size(); /* 防止越界 */
    QScreen *screen = screens.value(m_screenIndex);
    if (screen)
    {
        m_screen_width = screen->size().width();
        m_screen_height = screen->size().height();
    }
    /* 验证分辨率参数 - 确保分辨率是偶数（H264要求） */
    if (dstW % 2 != 0 || dstH % 2 != 0)
    {
        m_dstW = dstW & ~1;
        m_dstH = dstH & ~1;
        LOG_WARN("Adjusting resolution from {}x{} to {}x{} to make it even for H264 compatibility", dstW, dstH, m_dstW, m_dstH);
    }
    else
    {
        m_dstW = dstW;
        m_dstH = dstH;
    }
    /* 以分辨率/场景经验值设置目标码率（单位：kbps）——不要用 width*height*fps 直接作为 kbps */
    if (targetKbps <= 0)
    {
        if (m_dstW <= 854 && m_dstH <= 480)
            targetKbps = 1800;
        else if (m_dstW <= 1280 && m_dstH <= 720)
            targetKbps = 3500;
        else if (m_dstW <= 1920 && m_dstH <= 1080)
            targetKbps = 7000;
        else if (m_dstW <= 2560 && m_dstH <= 1440)
            targetKbps = 12000;
        else
            targetKbps = 18000;
    }

#if QT_POINTER_SIZE == 4
    /* 32 位控制/被控链路更容易被 CPU 编码、解码和 TURN 带宽打满。 */
    /* RTP 一旦丢 H264 分片就会污染后续 P 帧参考链，先把码率压到更稳的范围，减少马赛克源头。 */
    if (m_dstW <= 1280 && m_dstH <= 720)
        targetKbps = std::min(targetKbps, 4000);
    else if (m_dstW <= 1920 && m_dstH <= 1080)
        targetKbps = std::min(targetKbps, 8000);
    else
        targetKbps = std::min(targetKbps, 12000);
#endif

    m_bitrate = targetKbps * 1000; /* 转换为 bps */
    /* 优先尝试硬件加速 */
    static QSet<QString> failedHardwareEncoders;
    QStringList skipped;
    for (std::shared_ptr<CodecInfo> codecInfo : FFmpegUtil->getH264Encoders())
    {
        if (!codecInfo)
            continue;
        if (codecInfo->isHardware && failedHardwareEncoders.contains(codecInfo->name))
        {
            skipped << codecInfo->name;
            continue;
        }
        cleanup();
        m_codecInfo = codecInfo;
        LOG_INFO("Trying acceleration: {}", codecInfo->toString());
        m_initialized = initializeCodec(codecInfo);

        if (m_initialized)
        {
            break;
        }
        if (codecInfo->isHardware)
        {
            failedHardwareEncoders.insert(codecInfo->name);
            LOG_WARN("✗ Failed to initialize {} acceleration, will skip on subsequent retries", codecInfo->name);
        }
        else
        {
            LOG_WARN("✗ Failed to initialize {} encoder, trying next", codecInfo->name);
        }
    }

    if (!skipped.isEmpty())
    {
        LOG_INFO("Skipped previously-failed hardware H264 encoders: {}",
                 skipped.join(QStringLiteral(", ")).toStdString());
    }

    if (m_initialized)
    {
        LOG_INFO("✓ Successfully initialized H264 encoder with {} acceleration", m_codecInfo->name);
    }
    else
    {
        LOG_ERROR("❌ Failed to initialize H264 encoder with any method");
        cleanup();
    }
    return m_initialized;
}

bool H264Encoder::updateRateControl(int fps, int targetKbps, bool forceAllKeyframes)
{
    QMutexLocker locker(&m_mutex);
    if (!m_initialized || !m_codecContext)
        return false;

    fps = qBound(1, fps, 120);
    if (targetKbps <= 0)
    {
        if (m_dstW <= 854 && m_dstH <= 480)
            targetKbps = 1800;
        else if (m_dstW <= 1280 && m_dstH <= 720)
            targetKbps = 3500;
        else if (m_dstW <= 1920 && m_dstH <= 1080)
            targetKbps = 7000;
        else if (m_dstW <= 2560 && m_dstH <= 1440)
            targetKbps = 12000;
        else
            targetKbps = 18000;
    }

#if QT_POINTER_SIZE == 4
    if (m_dstW <= 1280 && m_dstH <= 720)
        targetKbps = std::min(targetKbps, 4000);
    else if (m_dstW <= 1920 && m_dstH <= 1080)
        targetKbps = std::min(targetKbps, 8000);
    else
        targetKbps = std::min(targetKbps, 12000);
#endif

    const int64_t bitrate = static_cast<int64_t>(targetKbps) * 1000;
    if (m_fps == fps && m_bitrate == bitrate && m_forceAllKeyframes == forceAllKeyframes)
        return true;

    m_fps = fps;
    m_bitrate = bitrate;
    m_forceAllKeyframes = forceAllKeyframes;

    m_codecContext->bit_rate = m_bitrate;
    m_codecContext->rc_max_rate = m_bitrate;
    m_codecContext->rc_min_rate = std::max<int64_t>(100000, m_bitrate * 8 / 10);
    m_codecContext->rc_buffer_size = std::max<int64_t>(m_bitrate / 2, 500000);
    m_codecContext->time_base = AVRational{1, m_fps};
    m_codecContext->framerate = AVRational{m_fps, 1};
    m_codecContext->gop_size = std::max(1, m_fps);
    m_codecContext->keyint_min = std::max(1, m_fps / 2);

    if (m_codecContext->priv_data)
    {
        av_opt_set_int(m_codecContext->priv_data, "b", m_bitrate, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(m_codecContext->priv_data, "bitrate", m_bitrate, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(m_codecContext->priv_data, "maxrate", m_bitrate, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(m_codecContext->priv_data, "bufsize", std::max<int64_t>(m_bitrate / 2, 500000), AV_OPT_SEARCH_CHILDREN);
    }

    m_forceNextKeyframe = true;
    LOG_INFO("Updated encoder rate control in-place: fps={}, bitrate={}kbps, allKeyframes={}",
             m_fps, m_bitrate / 1000, m_forceAllKeyframes);
    return true;
}

void H264Encoder::requestKeyframe()
{
    QMutexLocker locker(&m_mutex);
    m_forceNextKeyframe = true;
    LOG_INFO("Next encoded H264 frame will be forced as keyframe");
}

void H264Encoder::prepareFrameForEncode(AVFrame *frame)
{
    if (!frame)
        return;

    frame->pts = m_pts++;
    if (m_forceNextKeyframe || m_forceAllKeyframes)
    {
        frame->pict_type = AV_PICTURE_TYPE_I;
#if AIRAN_FFMPEG_HAS_FRAME_FLAGS
        frame->flags |= AV_FRAME_FLAG_KEY;
#else
        frame->key_frame = 1;
#endif
        if (m_forceNextKeyframe)
        {
            m_forceNextKeyframe = false;
            LOG_INFO("Forced current H264 frame as keyframe");
        }
    }
}
