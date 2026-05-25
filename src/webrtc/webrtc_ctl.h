#ifndef WEBRTC_CTL_H
#define WEBRTC_CTL_H

#include <QObject>
#include <QThread>
#include <QEventLoop>
#include <QJsonObject>
#include <QMessageBox>
#include <QMetaEnum>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMutex>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImage>
#include <QTimer>
#include <QStringList>
#include <QMap>
#include <QVector>
#include <QQueue>
#include <QDir>
#include <QDirIterator>
#include <QSettings>
#include <QUuid>
#include <QSet>
#include <memory>
#include <atomic>
#include <chrono>
#include "../util/input_util.h"
#include "../util/json_util.h"
#include "../common/constant.h"

class H264Decoder;
class FilePacketUtil;
class AudioPlaybackWorker;
class AudioCaptureWorker;

class WebRtcCtl : public QObject
{
    Q_OBJECT
public:
    WebRtcCtl(const QString &remoteId, const QString &remotePwdMd5, bool isOnlyFile, QObject *parent = nullptr);
    ~WebRtcCtl();

    void parseWsMsg(const QJsonObject &object);
    void init();

private:
    void initPeerConnection();
    void createTracks();
    void setupCallbacks();
    void setupFileChannelCallbacks();
    void setupFileTextChannelCallbacks();
    void setupInputChannelCallbacks();
    void sendStreamConfig();
    void destroy();

    bool uploadSingleFile(const QString &ctlPath, const QString &cliPath, const QString &transferId,
                          qint64 baseBytes = 0, qint64 totalBytes = -1, int currentFileIndex = 1, int totalFiles = 1);
    void uploadDirectory(const QString &ctlPath, const QString &cliPath, const QString &transferId);
    bool sendFileMetadataPacket(const QJsonObject &header, const QString &transferId = QString());
    bool isTransferCancelled(const QString &transferId) const;
    void markTransferCancelled(const QString &transferId);
    void clearTransferCancelled(const QString &transferId);
    void sendTransferCancel(const QString &transferId);
    void emitTransferProgress(const QString &transferId, qint64 transferredBytes, qint64 totalBytes, int transferredFiles, int totalFiles);
    qint64 collectDirectoryStats(const QString &path, int *fileCount) const;
    void addRemoteCandidateOrQueue(const QString &candidate, const QString &mid);
    void flushPendingRemoteCandidates();

    void processVideoFrame(const rtc::binary &videoData, const rtc::FrameInfo &frameInfo);
    void processAudioFrame(const rtc::binary &audioData, const rtc::FrameInfo &frameInfo);
    bool isH264Keyframe(const rtc::binary &data) const;
    bool shouldDecodeVideoFrame(bool isKeyframe, const char *context);
    bool enqueueVideoFrame(rtc::binary videoData, const rtc::FrameInfo &frameInfo);
    bool pruneVideoFrameQueueLocked();
    void pruneReliableVideoFragmentsLocked(qint64 nowMs);
    void startWaitingForVideoKeyframe(const char *reason);
    void requestRemoteKeyframe(const char *reason);
    void maybeSendVideoAdaptFeedback(bool congested, bool stalled, const char *reason);
    void noteVideoFrameArrival(qint64 nowMs, qint64 bytes, quint64 timestampUs);
    void resetVideoReceivePipeline(const char *reason, bool waitForKeyframe);
    void setupVideoDataChannelCallbacks();
    bool processReliableVideoMessage(const rtc::binary &message);
    int scanLengthPrefixedH264(const rtc::binary &data, size_t lengthSize) const;
    rtc::Configuration buildRtcConfiguration() const;
    void applyLocalStreamConfig(const QJsonObject &object);
    void noteLocalNetworkCandidate(const QString &candidate);
    void publishNetworkPathState(const QString &selectedPath = QString());
    void restartAfterNetworkPathChange();
    void startAudioPlayback();
    void stopAudioPlayback();
    void startAudioCapture();
    void stopAudioCapture();
    bool ensureAudioTrack();
    quint64 normalizeAudioTimestampUs(quint64 timestamp_us);

    void onPeerConnectionStateChanged(rtc::PeerConnection::State state);
    void onPeerIceStateChanged(rtc::PeerConnection::IceState state);
    void onPeerGatheringStateChanged(rtc::PeerConnection::GatheringState state);
    void onPeerLocalDescription(rtc::Description description);
    void onPeerLocalCandidate(const rtc::Candidate &candidate);
    void onVideoFrameReceived(rtc::binary data, rtc::FrameInfo info);
    void onAudioFrameReceived(rtc::binary data, rtc::FrameInfo info);
    void onAudioFrameReady(std::shared_ptr<rtc::binary> encodedData, quint64 timestamp_us);
    void onRemoteTrack(std::shared_ptr<rtc::Track> track);
    void onRemoteDataChannel(std::shared_ptr<rtc::DataChannel> channel);
    void onVideoDataChannelOpen();
    void onVideoDataChannelClosed();
    void onVideoDataChannelError(const std::string &error);
    void onVideoDataChannelMessage(const rtc::message_variant &message);
    void onFileChannelOpen();
    void onFileChannelClosed();
    void onFileChannelError(const std::string &error);
    void onFileChannelMessage(const rtc::message_variant &message);
    void onFileTextChannelOpen();
    void onFileTextChannelClosed();
    void onFileTextChannelError(const std::string &error);
    void onFileTextChannelMessage(const rtc::message_variant &message);
    void onInputChannelOpen();
    void onInputChannelClosed();
    void onInputChannelError(const std::string &error);
    void onInputChannelMessage(const rtc::message_variant &message);

    struct QueuedVideoFrame
    {
        rtc::binary data;
        rtc::FrameInfo info{std::chrono::duration<double>(0)};
        bool keyframe = false;
    };

    QString m_remoteId;
    QString m_remotePwdMd5;
    bool m_isOnlyFile;
    std::shared_ptr<rtc::PeerConnection> m_peerConnection;
    std::shared_ptr<rtc::DataChannel> m_fileChannel;
    std::shared_ptr<rtc::DataChannel> m_fileTextChannel;
    std::shared_ptr<rtc::DataChannel> m_inputChannel;
    std::shared_ptr<rtc::DataChannel> m_videoDataChannel;
    std::shared_ptr<rtc::Track> m_videoTrack;
    std::shared_ptr<rtc::Track> m_audioTrack;
    QThread *m_audioPlaybackThread{nullptr};
    AudioPlaybackWorker *m_audioPlaybackWorker{nullptr};
    QThread *m_audioCaptureThread{nullptr};
    AudioCaptureWorker *m_audioCaptureWorker{nullptr};

    bool m_connected = false;
    bool needSendAnswer = false;
    bool m_remoteDescriptionSet = false;
    QVector<QPair<QString, QString>> m_pendingRemoteCandidates;

    std::unique_ptr<FilePacketUtil> m_filePacketUtil;

    std::string m_host;
    uint16_t m_port = 0;
    std::string m_username;
    std::string m_password;
    QString m_streamMode{QStringLiteral("smooth")};
    QString m_bitrateProfile{QStringLiteral("medium")};
    QString m_networkPath{QStringLiteral("auto")};
    QString m_captureBackend{QStringLiteral("auto")};
    int m_requestedWidth{0};
    int m_requestedHeight{0};
    QStringList m_availableNetworkPaths{QStringLiteral("auto")};
    QString m_selectedNetworkPath;
    QString m_pendingNetworkPathReconnect;
    QString m_audioMode{QStringLiteral("off")};
    quint64 m_lastAudioTimestampUs{0};
    bool m_hasLastAudioTimestamp{false};

    std::unique_ptr<H264Decoder> m_h264Decoder;

    rtc::binary m_h264FrameBuffer;
    QMutex m_h264BufferMutex;

    std::chrono::steady_clock::time_point m_lastKeyframeRequestTime{};
    int m_keyframeRequestBackoffMs = 1000;
    std::atomic_bool m_waitingForVideoKeyframe{true};
    qint64 m_videoKeyframeWaitStartedMs = 0;

    QMutex m_videoQueueMutex;
    QQueue<QueuedVideoFrame> m_videoFrameQueue;
    bool m_videoDrainScheduled = false;
    QMap<quint32, QVector<rtc::binary>> m_videoFragmentBuffer;
    QMap<quint32, int> m_videoFragmentReceivedCount;
    QMap<quint32, quint64> m_videoFragmentTimestampUs;
    QMap<quint32, qint64> m_videoFragmentFirstSeenMs;
    QMutex m_videoReliableMutex;
    quint32 m_lastAcceptedReliableVideoSeq = 0;
    bool m_hasLastAcceptedReliableVideoSeq = false;
    std::atomic_bool m_acceptReliableVideo{false};

    QTimer *m_reconnectTimer = nullptr;
    int m_reconnectAttempts = 0;
    int m_reconnectBackoffMs = 5000;
    bool m_allowReconnect = true;
    std::atomic_bool m_reconnectPending{false};
    bool m_shutdownDone = false;

    qint64 m_lastInputMoveSendMs = 0;
    qint64 m_lastInputNotReadyLogMs = 0;
    QTimer *m_controlHeartbeatTimer = nullptr;

    qint64 m_videoStatsStartMs = 0;
    qint64 m_videoStatsBytes = 0;
    qint64 m_lastVideoAdaptFeedbackMs = 0;
    qint64 m_lastVideoDecodedMs = 0;
    qint64 m_lastVideoPacketMs = 0;
    qint64 m_firstVideoPacketWithoutDecodeMs = 0;
    qint64 m_lastVideoWatchdogMs = 0;
    int m_videoFeedbackDecodedFrames = 0;
    int m_videoFeedbackDroppedFrames = 0;
    int m_videoFeedbackKeyframeRequests = 0;
    qint64 m_videoArrivalWindowStartMs = 0;
    qint64 m_videoArrivalLastMs = 0;
    qint64 m_videoArrivalBytes = 0;
    int m_videoArrivalFrames = 0;
    double m_videoArrivalAvgGapMs = 0.0;
    double m_videoArrivalJitterMs = 0.0;
    quint64 m_videoArrivalLastTimestampUs = 0;
    QString m_lastRuntimeDiagnostics;
    mutable QMutex m_transferMutex;
    QSet<QString> m_cancelledTransfers;

private slots:
    void drainVideoFrameQueue();
    void doReconnect();
    void sendControlHeartbeat();

signals:
    void sendWsCliBinaryMsg(const QByteArray &message);
    void sendWsCliTextMsg(const QString &message);
    void runtimeDiagnosticsUpdated(const QString &diagnostics);
    void remoteCaptureBackendsChanged(const QStringList &backends, const QString &currentBackend);
    void remoteEncoderChanged(const QString &encoderName, const QString &encoderType, bool zeroCopy);
    void remoteStreamModeChanged(const QString &mode);
    void remoteOsChanged(const QString &osName);
    void networkPathStateChanged(const QStringList &availablePaths, const QString &selectedPath, const QString &requestedPath);
    void desktopStateChanged(bool locked, const QString &message);

    void recvGetFileList(const QJsonObject &object);
    void recvDownloadFile(bool status, const QString &filePath);
    void recvUploadFileRes(bool status, const QString &filePath);
    void fileTransferProgress(const QString &transferId, qint64 transferredBytes, qint64 totalBytes, int transferredFiles, int totalFiles);
    void fileTextChannelOpened();
    void terminalOutput(const QByteArray &data);
    void terminalClosed(int exitCode);
    void terminalError(const QString &message);
    void terminalInfo(const QString &osName, const QString &shellPath, const QString &mode, bool pathTracking);

    void videoFrameDecoded(const QImage &frame);
    void videoStatsUpdated(double kbps, const QSize &resolution);

public slots:
    void onWsCliRecvBinaryMsg(const QByteArray &message);
    void onWsCliRecvTextMsg(const QString &message);
    void shutdown();
    void scheduleReconnect();
    void stopReconnect();

    void inputChannelSendMsg(const rtc::message_variant &data);
    void fileChannelSendMsg(const rtc::message_variant &data);
    void fileTextChannelSendMsg(const rtc::message_variant &data);
    void uploadFile2CLI(const QString &ctlPath, const QString &cliPath, const QString &transferId);
    void cancelFileTransfer(const QString &transferId);
    void setNetworkPath(const QString &networkPath);
    void setAudioMode(const QString &mode);
};

#endif /* WEBRTC_CTL_H */
