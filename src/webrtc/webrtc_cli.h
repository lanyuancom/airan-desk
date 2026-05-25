#ifndef WEBRTC_CLI_H
#define WEBRTC_CLI_H

#include <QThread>
#include <QEventLoop>
#include <QJsonObject>
#include <QMetaEnum>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMutex>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImage>
#include <QSettings>
#include <QDir>
#include <QDirIterator>
#include <QRect>
#include <QHostInfo>
#include <QScreen>
#include <QMessageBox>
#include <QMetaObject>
#include <QGuiApplication>
#include <QTimer>
#include <QDateTime>
#include <QUuid>
#include <QSet>
#include <memory>
#include "../util/input_util.h"
#include "../common/constant.h"
#include "../util/json_util.h"

class DesktopGrab;
class FilePacketUtil;
class DesktopCaptureManager;
class TerminalSession;
class AudioCaptureWorker;
class AudioPlaybackWorker;

class WebRtcCli : public QObject
{
    Q_OBJECT
public:
    WebRtcCli(const QString &remoteId, int fps, bool isOnlyFile, QObject *parent = nullptr);
    WebRtcCli(const QString &remoteId, int fps, bool isOnlyFile, int requestedWidth, int requestedHeight,
              const QString &qualityMode = QStringLiteral("quality"),
              const QString &bitrateProfile = QStringLiteral("medium"),
              const QString &networkPath = QStringLiteral("auto"), QObject *parent = nullptr);
    ~WebRtcCli();

    void parseWsMsg(const QJsonObject &object);

private:
    void initPeerConnection();
    void createTracksAndChannels();
    void setupCallbacks();

    rtc::Reliability createVideoDataReliability() const;
    bool isH264Keyframe(const rtc::binary &data) const;
    bool trySendReliableVideoFrame(const rtc::binary &frame, quint64 timestamp_us);
    bool sendReliableVideoFrameFragmented(const rtc::binary &frame, quint64 timestamp_us);
    void flushPendingVideoKeyframe();
    void setStreamMode(const QString &mode);
    int effectiveBitrateKbps() const;
    int effectiveCaptureFps() const;
    bool forceAllKeyframes() const;
    rtc::Configuration buildRtcConfiguration() const;
    quint64 normalizeVideoTimestampUs(quint64 timestamp_us);
    quint64 normalizeAudioTimestampUs(quint64 timestamp_us);
    void switchToNextScreen();

    QString m_remoteId;
    QString m_subscriberId;
    bool m_isOnlyFile;
    QDir m_currentDir;

    std::shared_ptr<rtc::PeerConnection> m_peerConnection;
    std::shared_ptr<rtc::DataChannel> m_fileChannel;
    std::shared_ptr<rtc::DataChannel> m_fileTextChannel;
    std::shared_ptr<rtc::DataChannel> m_inputChannel;
    std::shared_ptr<rtc::DataChannel> m_videoDataChannel;
    std::shared_ptr<rtc::Track> m_videoTrack;
    std::shared_ptr<rtc::Track> m_audioTrack;
    QThread *m_audioCaptureThread{nullptr};
    AudioCaptureWorker *m_audioCaptureWorker{nullptr};
    QThread *m_audioPlaybackThread{nullptr};
    AudioPlaybackWorker *m_audioPlaybackWorker{nullptr};

    bool m_connected = false;
    bool m_channelsReady = false;
    bool m_destroying = false;
    bool m_remoteDescriptionSet = false;
    QVector<QPair<QString, QString>> m_pendingRemoteCandidates;

    int m_fps = 0;
    bool m_subscribed = false;
    quint64 m_lastTimestamp = 0;
    bool m_hasLastTimestamp = false;
    quint64 m_lastAudioTimestampUs = 0;
    bool m_hasLastAudioTimestamp = false;
    qint64 m_lastTimestampWarnMs = 0;
    int m_timestampWarnSuppressed = 0;
    QString m_lastStreamConfigSignature;
    bool m_videoDataCongested = false;
    quint32 m_videoFrameSeq = 0;
    std::shared_ptr<rtc::binary> m_pendingVideoKeyframe;
    qint64 m_videoPacerLastRefillMs = 0;
    qint64 m_videoPacerTokens = 0;
    int m_videoPacerDroppedFrames = 0;
    qint64 m_lastVideoPacerDropLogMs = 0;
    qint64 m_lastVideoPacerAdaptMs = 0;
    QString m_streamMode{QStringLiteral("smooth")};
    bool m_qualityFirstMode = false;
    QString m_bitrateProfile{QStringLiteral("medium")};
    QString m_networkPath{QStringLiteral("auto")};
    bool m_compatMode = false;
    int m_baseFps = 0;
    QString m_baseBitrateProfile{QStringLiteral("medium")};
    int m_baseRequestedEncodeWidth = 0;
    int m_baseRequestedEncodeHeight = 0;
    int m_adaptLevel = 0;
    int m_stableVideoFeedbacks = 0;
    qint64 m_lastVideoAdaptMs = 0;
    double m_feedbackArrivalKbpsEwma = 0.0;
    double m_feedbackArrivalKbpsVar = 0.0;
    double m_feedbackJitterMsEwma = 0.0;
    double m_feedbackJitterMsVar = 0.0;
    double m_feedbackDecodeQueueEwma = 0.0;
    double m_feedbackDecodeQueueVar = 0.0;
    double m_feedbackLossRateEwma = 0.0;
    double m_feedbackLossRateVar = 0.0;

    FilePacketUtil *m_filePacketUtil = nullptr;
    TerminalSession *m_terminalSession = nullptr;

    std::string m_host;
    uint16_t m_port = 0;
    std::string m_username;
    std::string m_password;
    int m_screen_width = 0;
    int m_screen_height = 0;
    int m_screenIndex = 0;

    int m_encode_width = 0;
    int m_encode_height = 0;
    int m_requestedEncodeWidth = 0;
    int m_requestedEncodeHeight = 0;
    QString m_captureBackend{QStringLiteral("auto")};
    QString m_encoderName;
    bool m_encoderIsHardware{false};
    bool m_encoderZeroCopy{false};
    bool m_desktopLocked{false};
    bool m_audioCaptureEnabled{false};
    QString m_audioMode{QStringLiteral("off")};

    QTimer *m_inputChannelRecoverTimer = nullptr;
    QTimer *m_videoDataChannelRecoverTimer = nullptr;
    QTimer *m_controlWatchdogTimer = nullptr;
    QTimer *m_disconnectGraceTimer = nullptr;
    qint64 m_lastControlAliveMs = 0;

    QMap<QString, QVector<QByteArray>> m_uploadFragments;
    mutable QMutex m_transferMutex;
    QSet<QString> m_cancelledTransfers;

signals:
    void sendWsCliBinaryMsg(const QByteArray &message);
    void sendWsCliTextMsg(const QString &message);
    void destroyCli();

public slots:
    void init();
    void destroy();
    void onWsCliRecvBinaryMsg(const QByteArray &message);
    void onWsCliRecvTextMsg(const QString &message);
    void onVideoFrameReady(const QString &subscriberId, std::shared_ptr<rtc::binary> encodedData, quint64 timestamp_us);
    void onAudioFrameReady(std::shared_ptr<rtc::binary> encodedData, quint64 timestamp_us);
    void handleFileReceived(bool status, const QString &tempPath);
    void recoverInputChannel();
    void recoverVideoDataChannel();
    void checkControlAlive();
    void setDesktopLocked(bool locked);

private slots:
    void startMediaCapture();
    void stopMediaCapture();
    void setupFileChannelCallbacks();
    void setupFileTextChannelCallbacks();
    void setupInputChannelCallbacks();
    void setupVideoDataChannelCallbacks();
    void scheduleInputChannelRecovery(const QString &reason);
    void scheduleVideoDataChannelRecovery(const QString &reason);
    void notifyCurrentStreamMode();
    void notifyDesktopState();
    void setAudioCaptureEnabled(bool enabled);
    void setAudioMode(const QString &mode);
    void onCaptureBackendConfirmed(int screenIndex, const QString &requestedBackend, const QString &actualBackend);
    void onCaptureEncoderConfirmed(int screenIndex, const QString &encoderName, bool isHardware, bool zeroCopy);
    void parseFileMsg(const QJsonObject &object);
    void handleTerminalMessage(const QJsonObject &object);

private:
    void onPeerConnectionStateChanged(rtc::PeerConnection::State state);
    void onPeerIceStateChanged(rtc::PeerConnection::IceState state);
    void onPeerGatheringStateChanged(rtc::PeerConnection::GatheringState state);
    void onPeerLocalDescription(rtc::Description description);
    void onPeerLocalCandidate(const rtc::Candidate &candidate);
    void onVideoPliRequested();
    void onAudioFrameReceived(rtc::binary data, rtc::FrameInfo info);
    void onFileChannelOpen();
    void onFileChannelMessage(rtc::message_variant data);
    void onFileChannelError(std::string error);
    void onFileChannelClosed();
    void onFileTextChannelOpen();
    void onFileTextChannelMessage(rtc::message_variant data);
    void onFileTextChannelError(std::string error);
    void onFileTextChannelClosed();
    void onInputChannelOpen();
    void onInputChannelMessage(rtc::message_variant data);
    void onInputChannelError(std::string error);
    void onInputChannelClosed();
    void onVideoDataChannelOpen();
    void onVideoDataChannelBufferedAmountLow();
    void onVideoDataChannelClosed();
    void onVideoDataChannelError(std::string error);
    void onTerminalOutputReady(const QByteArray &data);
    void onTerminalClosed(int exitCode);
    void onTerminalError(const QString &message);
    void onTerminalInfoReady(const QString &osName, const QString &shellPath, const QString &mode, bool pathTracking);

    void parseInputMsg(const QJsonObject &object);

    void setRemoteDescription(const QString &data, const QString &type);
    void addIceCandidate(const QString &candidate, const QString &mid);

    void populateLocalFiles();

    void sendFile(const QString &cliPath, const QString &ctlPath, const QString &transferId);
    bool sendSingleFile(const QString &cliPath, const QString &ctlPath, const QString &transferId,
                        qint64 baseBytes = 0, qint64 totalBytes = -1, int currentFileIndex = 1, int totalFiles = 1);
    void sendDirectory(const QString &cliPath, const QString &ctlPath, const QString &transferId);
    bool sendFileMetadataPacket(const QJsonObject &header, const QString &transferId = QString());
    bool isTransferCancelled(const QString &transferId) const;
    void markTransferCancelled(const QString &transferId);
    void clearTransferCancelled(const QString &transferId);
    void sendTransferProgress(const QString &transferId, qint64 transferredBytes, qint64 totalBytes, int transferredFiles, int totalFiles,
                              const QString &ctlPath = QString(), const QString &cliPath = QString());
    void sendTransferCancel(const QString &transferId);
    qint64 collectDirectoryStats(const QString &path, int *fileCount) const;
    void addRemoteCandidateOrQueue(const QString &candidate, const QString &mid);
    void flushPendingRemoteCandidates();
    void sendFileErrorResponse(const QString &filePath, const QString &error);
    void sendUploadResponse(const QString &fileName, bool success, const QString &message);
    void saveUploadedFile(const QString &filePath, const QByteArray &data);

    void handleMouseEvent(const QJsonObject &object);
    void handleKeyboardEvent(const QJsonObject &object);
    void handleStreamConfig(const QJsonObject &object);
    void handleAudioCaptureConfig(const QJsonObject &object);
    void handleSwitchScreen(const QJsonObject &object);
    void handleRemoteOperation(const QJsonObject &object);
    void handleVideoAdaptFeedback(const QJsonObject &object);
    void updateFeedbackEwma(double sample, double &mean, double &variance, double alpha = 0.18);
    void handleRunFile(const QJsonObject &object);
    void applyCaptureBackend(const QString &backend);
    void applyRequestedResolution(int requestedWidth, int requestedHeight);
    void applyVideoAdaptation();
    bool raiseVideoAdaptLevel(const QString &reason, int step = 1);
    void resetVideoPacer();

    void sendFileChannelMessage(const QJsonObject &message);
    void sendFileTextChannelMessage(const QJsonObject &message);
    void sendInputChannelMessage(const QJsonObject &message);

    void calculateEncodeResolution(int requestedMaxWidth, int requestedMaxHeight);
    void startAudioCapture();
    void stopAudioCapture();
    void startAudioPlayback();
    void stopAudioPlayback();
};

#endif /* WEBRTC_CLI_H */
