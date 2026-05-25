#ifndef CODEC_INFO_H
#define CODEC_INFO_H

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/version.h>
#include <libavfilter/version.h>
}

#if defined(LIBAVCODEC_VERSION_INT) && LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 18, 100)
#define AIRAN_FFMPEG_HAS_HWCONFIG (1)
#else
#define AIRAN_FFMPEG_HAS_HWCONFIG (0)
#endif

/*
 * AVHWDeviceType 来自 libavutil/hwcontext.h。注意 AV_HWDEVICE_TYPE_* 是 enum
 * 常量，不是预处理宏，不能用 #ifdef AV_HWDEVICE_TYPE_NONE 判断是否存在。
 * Ubuntu18.04 FFmpeg3.4(libavutil55) 已提供该类型，只是没有
 * AVCodecHWConfig/avcodec_get_hw_config，所以仅在更老的 libavutil 下兜底。
 */
#if defined(LIBAVUTIL_VERSION_MAJOR) && LIBAVUTIL_VERSION_MAJOR >= 55
#define AIRAN_FFMPEG_HAS_HWDEVICE (1)
#else
#define AIRAN_FFMPEG_HAS_HWDEVICE (0)
#endif

#if defined(LIBAVFILTER_VERSION_INT) && LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(7, 16, 100)
#define AIRAN_FFMPEG_HAS_BUFFERSINK_HW_FRAMES_CTX (1)
#else
#define AIRAN_FFMPEG_HAS_BUFFERSINK_HW_FRAMES_CTX (0)
#endif

#if defined(LIBAVCODEC_VERSION_INT) && LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 37, 100)
#define AIRAN_FFMPEG_HAS_SEND_RECEIVE_API (1)
#else
#define AIRAN_FFMPEG_HAS_SEND_RECEIVE_API (0)
#endif

#if defined(LIBAVUTIL_VERSION_INT) && LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
#define AIRAN_FFMPEG_HAS_FRAME_FLAGS (1)
#else
#define AIRAN_FFMPEG_HAS_FRAME_FLAGS (0)
#endif

#if defined(LIBAVUTIL_VERSION_INT) && LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#define AIRAN_FFMPEG_HAS_CH_LAYOUT (1)
#else
#define AIRAN_FFMPEG_HAS_CH_LAYOUT (0)
#endif

#if defined(LIBAVCODEC_VERSION_INT) && LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
#define AIRAN_FFMPEG_HAS_SUPPORTED_CONFIG (1)
#else
#define AIRAN_FFMPEG_HAS_SUPPORTED_CONFIG (0)
#endif

#if !AIRAN_FFMPEG_HAS_HWDEVICE
typedef int AVHWDeviceType;
static const AVHWDeviceType AV_HWDEVICE_TYPE_NONE = 0;
static const AVHWDeviceType AV_HWDEVICE_TYPE_VDPAU = 1;
static const AVHWDeviceType AV_HWDEVICE_TYPE_CUDA = 2;
static const AVHWDeviceType AV_HWDEVICE_TYPE_VAAPI = 3;
static const AVHWDeviceType AV_HWDEVICE_TYPE_DXVA2 = 4;
static const AVHWDeviceType AV_HWDEVICE_TYPE_QSV = 5;
static const AVHWDeviceType AV_HWDEVICE_TYPE_VIDEOTOOLBOX = 6;
static const AVHWDeviceType AV_HWDEVICE_TYPE_D3D11VA = 7;
#endif

/*
 * FFmpeg 的 AV_HWDEVICE_TYPE_* 是 enum 常量，不是预处理宏；旧 3.x 头文件
 * 缺少较新的枚举值时不能用 #ifdef 判断。项目内部统一用这些稳定数值常量，
 * 避免直接引用旧头文件不存在的枚举名。
 */
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_NONE = static_cast<AVHWDeviceType>(0);
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_VDPAU = static_cast<AVHWDeviceType>(1);
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_CUDA = static_cast<AVHWDeviceType>(2);
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_VAAPI = static_cast<AVHWDeviceType>(3);
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_DXVA2 = static_cast<AVHWDeviceType>(4);
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_QSV = static_cast<AVHWDeviceType>(5);
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_VIDEOTOOLBOX = static_cast<AVHWDeviceType>(6);
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_D3D11VA = static_cast<AVHWDeviceType>(7);
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_DRM = static_cast<AVHWDeviceType>(8);
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_OPENCL = static_cast<AVHWDeviceType>(9);
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_MEDIACODEC = static_cast<AVHWDeviceType>(10);
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_VULKAN = static_cast<AVHWDeviceType>(11);
static constexpr AVHWDeviceType AIRAN_AV_HWDEVICE_TYPE_D3D12VA = static_cast<AVHWDeviceType>(12);

#include <QString>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <memory>

struct HwAccelInfo
{
    int score = 0;
#if AIRAN_FFMPEG_HAS_HWCONFIG
    const AVCodecHWConfig *config = nullptr;
#endif
    AVHWDeviceType hwDeviceType = AIRAN_AV_HWDEVICE_TYPE_NONE;
    QString hwDeviceTypeName;
    QList<AVPixelFormat> supportedPixFormats;
    QList<QString> supportedPixFormatNames;

    QJsonObject toJson() const
    {
        QJsonObject jsonObj;
        jsonObj["hwDeviceType"] = static_cast<int>(hwDeviceType);
        jsonObj["hwDeviceTypeName"] = hwDeviceTypeName;
        QJsonArray pixFormatsArray;
        for (const auto &pixFmtName : supportedPixFormatNames)
        {
            pixFormatsArray.append(pixFmtName);
        }
        jsonObj["supportedPixFormats"] = pixFormatsArray;
        return jsonObj;
    }
    QString toString()
    {
        return QJsonDocument(toJson()).toJson(QJsonDocument::Compact);
    }
};

class CodecInfo
{
public:
    enum Type
    {
        UNKNOWN = 0,
        ENCODER = 1,
        DECODER = 2
    };
    int score = 0;
    QString name;
    QString longName;
    /* 0: unknown, 1: encoder, 2: decoder */
    Type type = UNKNOWN;
    /* 是否硬件加速 */
    bool isHardware = false;
    /* ffmpeg codec pointer */
    const AVCodec *codec = nullptr;
    /* 支持的硬件类型 */
    QList<std::shared_ptr<HwAccelInfo>> supportedHwTypes;

    CodecInfo() {}
    ~CodecInfo() { supportedHwTypes.clear(); }

    QJsonObject toJson()
    {
        QJsonObject jsonObj;
        jsonObj["name"] = name;
        jsonObj["longName"] = longName;
        jsonObj["type"] = static_cast<int>(type);
        jsonObj["isHardware"] = isHardware;
        QJsonArray hwTypesArray;
        for (const auto &hwAccelInfo : supportedHwTypes)
        {
            hwTypesArray.append(hwAccelInfo->toJson());
        }
        jsonObj["supportedHwTypes"] = hwTypesArray;
        return jsonObj;
    }

    QString toString()
    {
        return QJsonDocument(toJson()).toJson(QJsonDocument::Compact);
    }
};
#endif /* CODEC_INFO_H */
