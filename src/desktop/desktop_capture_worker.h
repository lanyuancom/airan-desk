#ifndef DESKTOP_CAPTURE_WORKER_H
#define DESKTOP_CAPTURE_WORKER_H

#include <QObject>
#include <QMutex>
#include <QTimer>
#include <QMap>
#include <QThread>
#include <QElapsedTimer>
#include <memory>
#include "../codec/h264_encoder.h"

/* Forward declarations */
class DesktopGrab;

/**
 * @brief 捕获工作线程（运行在独立线程）
 *
 * 职责：
 * - 执行 DXGI/Qt 屏幕捕获
 * - 管理所有编码器实例
 * - 执行 H264 编码
 * - 发出编码完成信号
 */
class DesktopCaptureWorker : public QObject
{
    Q_OBJECT
public:
    explicit DesktopCaptureWorker(QObject *parent = nullptr);
    ~DesktopCaptureWorker();

public slots:
    /**
     * @brief 初始化捕获器
     */
    void initialize(int screenIndex, int fps);

    /**
     * @brief 添加订阅者
     */
    void addSubscriber(const QString &subscriberId, int dstW, int dstH, int fps, int targetKbps = 0, bool forceAllKeyframes = false);

    /**
     * @brief 移除订阅者
     */
    void removeSubscriber(const QString &subscriberId);

    /**
     * @brief 更新订阅者参数
     */
    void updateSubscriber(const QString &subscriberId, int dstW, int dstH, int fps, int targetKbps = 0, bool forceAllKeyframes = false);
    void setCaptureBackend(const QString &backend);
    void requestKeyframe(const QString &subscriberId);
    void retryAutoBackendPromotion();

    /**
     * @brief 停止采集并释放运行期资源
     */
    void stopCapture();

    /**
     * @brief 重新平衡捕获帧率
     */
    void reBalanceCaptureFps();

    /**
     * @brief 在对象所属线程中同步销毁自身，用于兼容不支持 functor invokeMethod 的旧 Qt 版本
     */
    void deleteInThread();
signals:
    /**
     * @brief 编码完成信号
     */
    void frameEncoded(const QString &subscriberId, std::shared_ptr<rtc::binary> encodedData, quint64 timestamp_us);

    /**
     * @brief 错误信号
     */
    void errorOccurred(const QString &errorMessage);
    void captureBackendChanged(int screenIndex, const QString &requestedBackend, const QString &actualBackend);
    void encoderChanged(int screenIndex, const QString &encoderName, bool isHardware, bool zeroCopy);

private slots:
    void onTimeout();

private:
    struct SubscriberInfo;

    bool captureAndEncodeGPU();
    bool captureAndEncodeCPU();
    bool ensureDesktopGrabberLocked();
    void promoteAutoBackendIfNeededLocked();
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    bool scaleTextureForSubscriberLocked(ID3D11Texture2D *srcTexture, SubscriberInfo *subscriber, ID3D11Texture2D *&outTexture);
#endif
    void resetDesktopGrabberLocked();
    void resetSubscriberGpuStateLocked(SubscriberInfo *subscriber);
    bool recreateSubscriberEncoderLocked(SubscriberInfo *subscriber);
    bool recreateSubscriberEncodersLocked();
    bool isZeroCopyActiveLocked(const H264Encoder *encoder) const;
    bool anySubscriberNeedsStaticCpuRefreshLocked(qint64 nowMs) const;

    void updateCaptureTimerLocked(int fps);
    void scheduleNextCaptureLocked(int delayMs = -1);
    void stopTimerLocked();

    struct SubscriberInfo
    {
        QString id;
        int dstW;
        int dstH;
        int fps;
        int targetKbps{0};
        bool forceAllKeyframes{false};
        std::unique_ptr<H264Encoder> encoder;
        bool zeroCopyActive{false};
        bool hasLastFrameFingerprint{false};
        quint64 lastFrameFingerprint{0};
        int unchangedFrames{0};
        qint64 lastEncodedMs{0};
        qint64 lastDirtyMs{0};
        bool forceNextFrame{false};
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
        ComPtr<ID3D11Device> scaleDevice;
        ComPtr<ID3D11VideoDevice> scaleVideoDevice;
        ComPtr<ID3D11VideoContext> scaleVideoContext;
        ComPtr<ID3D11VideoProcessorEnumerator> scaleVpEnum;
        ComPtr<ID3D11VideoProcessor> scaleProcessor;
        ComPtr<ID3D11Texture2D> scaleInputTex;
        ComPtr<ID3D11VideoProcessorInputView> scaleInputView;
        ComPtr<ID3D11Texture2D> scaleOutputTex;
        ComPtr<ID3D11VideoProcessorOutputView> scaleOutputView;
        UINT scaleSrcW{0};
        UINT scaleSrcH{0};
        DXGI_FORMAT scaleSrcFormat{DXGI_FORMAT_UNKNOWN};
#endif
    };

    QMutex m_mutex;
    QTimer *m_timer = nullptr;
    int m_screenIndex{0};
    int m_captureFps{30}; /* 当前采集帧率（取订阅者中的最大值） */
    bool m_captureInProgress{false};
    bool m_stopping{false};
    qint64 m_nextCaptureDueMs{0};
    QElapsedTimer m_gpuLogTimer;
    QElapsedTimer m_captureCostTimer;
    qint64 m_lastGpuGrabWarnMs{0};
    std::shared_ptr<DesktopGrab> m_desktopGrab;
    QMap<QString, SubscriberInfo *> m_subscribers;
    QString m_captureBackend{QStringLiteral("auto")};
    QString m_actualCaptureBackend;
    bool m_autoBackendPromotionPending{false};
};

#endif /* DESKTOP_CAPTURE_MANAGER_H */
