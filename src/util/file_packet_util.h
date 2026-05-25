#ifndef FILE_PACKET_UTIL_H
#define FILE_PACKET_UTIL_H

#include <QObject>
#include <QMutex>
#include <QDateTime>
#include <QUuid>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QDataStream>
#include <QThread>
#include <QJsonObject>
#include <QSet>
#include <memory>
#include <map>
#include <set>
#include <functional>
#include "../common/constant.h"

/* 分包相关常量 */
constexpr quint64 FRAGMENT_SIZE = 8 * 1024; /* 8KB */
constexpr quint64 HEADER_SIZE = 32;         /* 消息ID(16) + 总分包数(8) + 分包索引(8) */
constexpr quint64 PAYLOAD_SIZE = FRAGMENT_SIZE - HEADER_SIZE; /* 实际数据载荷大小 */
constexpr quint64 MAX_REASONABLE_OFFSET = 100LL * 1024 * 1024 * 1024; /* 100GB */

/* 重组缓冲区结构 */
struct ReassemblyBuffer {
    quint64 totalFragments = 0;
    QString tempFilePath;  /* 临时文件路径 */
    std::vector<bool> receivedFragments;  /* 标记哪些分片已接收 */
    qint64 timestamp = 0; /* 用于超时清理 */
    QFile* tempFile = nullptr;  /* 临时文件对象 */
    quint64 receivedBytes = 0;
    std::set<quint64> receivedIndexes;
};

class FilePacketUtil : public QObject
{
    Q_OBJECT

public:
    using ProgressCallback = std::function<void(qint64 sentBytes, qint64 totalBytes)>;
    using CancelCallback = std::function<bool()>;

    explicit FilePacketUtil(QObject *parent = nullptr);
    ~FilePacketUtil();

    /* 流式发送文件（避免大文件全部加载到内存） */
    static bool sendFileStream(const QString &filePath, const QJsonObject &header, std::shared_ptr<rtc::DataChannel> channel,
                               const ProgressCallback &progressCallback = ProgressCallback(),
                               const CancelCallback &cancelCallback = CancelCallback());
    static bool sendDataPacket(const QJsonObject &header, const QByteArray &payload, std::shared_ptr<rtc::DataChannel> channel,
                               const ProgressCallback &progressCallback = ProgressCallback(),
                               const CancelCallback &cancelCallback = CancelCallback());

    /* 处理接收到的分包数据 */
    void processReceivedFragment(const rtc::binary &data, const QString &channelName);

    /* 处理文件数据包（带头部信息的完整文件包） */
    void processFileDataPacket(const QString &tempFilePath);
    void cancelTransfer(const QString &transferId);
    void cancelAllReassemblies();

signals:
    /* 文件下载完成信号 */
    void fileDownloadCompleted(bool status, const QString &tempPath);

    /* 文件接收完成信号（通过分包重组） */
    void fileReceived(bool status, const QString &tempPath);

private:
    static bool waitForChannelBackpressure(const std::shared_ptr<rtc::DataChannel> &channel, const QString &filePath,
                                           const CancelCallback &cancelCallback);
    static bool sendPacketStream(QFile *file, const QByteArray &payload, const QJsonObject &header,
                                 std::shared_ptr<rtc::DataChannel> channel, const QString &logPath,
                                 const ProgressCallback &progressCallback, const CancelCallback &cancelCallback);
    void cleanupExpiredReassembliesLocked(qint64 nowMs);

    /* 重组分包 */
    void reassembleFragment(const QString &messageId, quint64 fragmentIndex,
                           quint64 totalFragments, const rtc::binary &fragment);

    /* 流式复制文件数据 */
    bool streamCopyFile(QFile &sourceFile, qint64 sourceOffset, const QString &targetPath, qint64 dataSize);

    /* 重组缓冲区映射表 */
    std::map<QString, ReassemblyBuffer> m_reassemblyBuffers;
    QSet<QString> m_cancelledTransfers;

    /* 重组互斥锁 */
    QMutex m_reassemblyMutex;
};

#endif /* FILE_PACKET_UTIL_H */
