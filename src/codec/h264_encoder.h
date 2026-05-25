#ifndef H264_ENCODER_H
#define H264_ENCODER_H

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include "../common/constant.h"
#include "../util/ffmpeg_util.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#ifndef AV_ERROR_MAX_STRING_SIZE
#define AV_ERROR_MAX_STRING_SIZE 64
#endif

class H264Encoder : public QObject
{
  Q_OBJECT

public:
  explicit H264Encoder(QObject *parent = nullptr);
  ~H264Encoder();

  /* 当前是否为硬件编码器实例 */
  bool isHardwareEncoder() const { return m_codecInfo && m_codecInfo->isHardware; }
  QString encoderName() const { return m_codecInfo ? m_codecInfo->name : QString(); }
  QString implementationName() const { return m_codecInfo ? m_codecInfo->name : QString(); }
  bool supportsZeroCopyEncoder() const { return m_zeroCopyHealthy && m_codecInfo && m_codecInfo->isHardware && m_hwaccelInfo && m_codecContext && m_codecContext->hw_frames_ctx; }
  bool isZeroCopyActive() const { return supportsZeroCopyEncoder() && m_zeroCopyHealthy; }
  void requestKeyframe();

  /* 初始化编码器 */
  bool initialize(int screenIndex, int dstW, int dstH, int fps = 30, int targetKbps = 0, bool forceAllKeyframes = false);
  bool updateRateControl(int fps, int targetKbps = 0, bool forceAllKeyframes = false);

  /* External encode interfaces used by DesktopCaptureWorker */
  std::pair<std::shared_ptr<rtc::binary>, quint64> encodeCPU(const QImage &in);
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
  std::pair<std::shared_ptr<rtc::binary>, quint64> encodeGPU(ID3D11Texture2D *in);
  /* 零拷贝编码：D3D11Texture2D -> 硬件设备零拷贝编码（支持 QSV/CUDA/VAAPI/D3D11VA 等） */
  std::pair<std::shared_ptr<rtc::binary>, quint64> zeroCopyEncodeGPU(ID3D11Texture2D *in);
#endif

  /* 释放资源 */
  void cleanup();

private:
  bool initializeCodec(std::shared_ptr<CodecInfo> codecInfo);
  bool initializeHardwareAccel(std::shared_ptr<CodecInfo> codecInfo);
  void prepareFrameForEncode(AVFrame *frame);

  bool convertToSwFormat(AVFrame *inputFrame, AVFrame *outputFrame, AVPixelFormat dstFormat); /* 通用系统内存像素格式转换 */
  int sendFrameToEncoder(AVFrame *frame, std::shared_ptr<rtc::binary> &out, quint64 &timestampUs);
  void appendAvailablePackets(std::shared_ptr<rtc::binary> &out, quint64 &timestampUs);
  AVFrame *createFrameFromQImage(const QImage &image);
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
  AVFrame *createFrameFromD3D11Texture(ID3D11Texture2D *texture);
  AVFrame *createHwFrameFromD3D11Texture(ID3D11Texture2D *texture);      /* 零拷贝：创建D3D11VA硬件帧 */
  bool reinitializeD3d11vaSessionForZeroCopy();                          /* 首帧后一次性重建NVENC(D3D11VA)会话 */
  bool reinitializeQsvSessionForZeroCopy();                              /* 首帧后一次性重建QSV会话 */
  bool reinitializeQsvCodecWithGraphFrames(AVBufferRef *graphFramesCtx); /* 用滤镜输出帧池再绑定一次QSV编码器 */
#endif

  AVCodecContext *m_codecContext;
  std::shared_ptr<CodecInfo> m_codecInfo;
  std::shared_ptr<HwAccelInfo> m_hwaccelInfo;
  AVPacket *m_packet;
  AVFrame *m_frame;
  AVFrame *m_hwFrame;
  SwsContext *m_swsContext;
  AVBufferRef *m_hwDeviceCtx;
  AVPixelFormat m_srcPixFmt;
  AVPixelFormat m_cachedSwsDstFmt;
  int m_cachedSwsSrcW;
  int m_cachedSwsSrcH;
  int m_cachedSwsDstW;
  int m_cachedSwsDstH;
  /* 目标 system-memory 像素格式（由 initializeHardwareAccel 决定，用于后续转换/上传） */
  AVPixelFormat m_targetSwFmt;
  /* 零拷贝编码支持 */
  AVBufferRef *m_d3d11vaDeviceCtx;     /* D3D11VA 硬件设备上下文（用于零拷贝） */
  AVBufferRef *m_d3d11vaFramesCtx;     /* 复用的 D3D11VA 帧上下文（避免每帧分配） */
  AVFilterGraph *m_filterGraph;        /* 滤镜图（用于硬件缩放） */
  AVFilterContext *m_bufferSrcCtx;     /* 滤镜源 */
  AVFilterContext *m_bufferSinkCtx;    /* 滤镜输出 */
  AVBufferRef *m_filterFramesCtx;      /* 当前滤镜图绑定的输入 hw_frames_ctx */
  int m_filterSrcW;                    /* 当前滤镜图输入宽 */
  int m_filterSrcH;                    /* 当前滤镜图输入高 */
  bool m_filterNeedScale;              /* 当前滤镜图是否包含缩放 */
  bool m_zeroCopyHealthy;              /* zero-copy 路径是否健康（失败后熔断） */
  static bool s_zeroCopyDisabledGlobally; /* 进程级永久禁用：当 D3D11VA hwframe 返回 ENOSYS 时整个 FFmpeg 构建都不支持零拷贝 */
  bool m_qsvDeriveChecked;             /* QSV 从 D3D11 派生能力是否已检查 */
  bool m_qsvDeriveOk;                  /* QSV 从 D3D11 派生能力检查结果 */
  AVBufferRef *m_forcedQsvDeviceCtx;   /* 首帧派生出的QSV设备上下文（用于一次性会话绑定） */
  AVBufferRef *m_forcedQsvFramesCtx;   /* 首帧滤镜输出QSV帧池（用于一次性会话绑定） */
  AVBufferRef *m_forcedD3d11DeviceCtx; /* 首帧绑定的D3D11VA设备上下文（用于NVENC会话绑定） */
  AVBufferRef *m_forcedD3d11FramesCtx; /* 首帧绑定的D3D11VA帧池（用于NVENC会话绑定） */
  bool m_qsvSessionBound;              /* QSV zero-copy会话是否已完成首帧绑定 */
  bool m_qsvFramesBound;               /* QSV编码器是否已绑定到滤镜输出帧池 */
  bool m_d3d11SessionBound;            /* NVENC(D3D11VA) zero-copy会话是否已完成首帧绑定 */

  /* 编码参数 */
  int m_screenIndex;
  int m_screen_width;
  int m_screen_height;
  int m_dstW;
  int m_dstH;
  int m_fps;
  int m_bitrate;
  int m_pts;
  bool m_forceAllKeyframes;

  /* 线程安全 */
  QMutex m_mutex;

  bool m_initialized;
  bool m_forceNextKeyframe;
  AVFormatContext *m_deviceCtx;
};

#endif /* H264_ENCODER_H */
