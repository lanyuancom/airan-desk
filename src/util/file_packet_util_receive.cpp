/* Split from file_packet_util.cpp by packet transfer responsibility. */

#include "file_packet_util.h"
#include "json_util.h"

namespace
{
constexpr qint64 kReassemblyTimeoutMs = 10 * 60 * 1000;
}

void FilePacketUtil::cleanupExpiredReassembliesLocked(qint64 nowMs)
{
    for (auto it = m_reassemblyBuffers.begin(); it != m_reassemblyBuffers.end();)
    {
        ReassemblyBuffer &buffer = it->second;
        if (buffer.timestamp > 0 && nowMs - buffer.timestamp > kReassemblyTimeoutMs)
        {
            LOG_WARN("Reassembly expired, remove temp file: {}", buffer.tempFilePath);
            if (buffer.tempFile)
            {
                buffer.tempFile->close();
                delete buffer.tempFile;
                buffer.tempFile = nullptr;
            }
            if (!buffer.tempFilePath.isEmpty())
                QFile::remove(buffer.tempFilePath);
            it = m_reassemblyBuffers.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void FilePacketUtil::processReceivedFragment(const rtc::binary &data, const QString &channelName)
{
    if (data.size() < HEADER_SIZE)
    {
        LOG_ERROR("Fragment too small: {}", ConvertUtil::formatFileSize(data.size()));
        return;
    }

    /* 解析包头 */
    QByteArray messageIdArray;
    messageIdArray.resize(16);
    for (int i = 0; i < 16; ++i)
    {
        messageIdArray[i] = static_cast<char>(data[i]);
    }
    QUuid messageId = QUuid::fromRfc4122(messageIdArray);

    if (messageId.isNull())
    {
        LOG_ERROR("Invalid message ID in fragment");
        return;
    }

    /* 解析总分包数（大端序，8字节） */
    quint64 totalFragments = 0;
    for (int i = 0; i < 8; ++i)
    {
        totalFragments = (totalFragments << 8) | static_cast<quint8>(data[16 + i]);
    }

    /* 解析分包索引（大端序，8字节） */
    quint64 fragmentIndex = 0;
    for (int i = 0; i < 8; ++i)
    {
        fragmentIndex = (fragmentIndex << 8) | static_cast<quint8>(data[24 + i]);
    }

    /* 验证解析结果的合理性 */
    if (totalFragments == 0 || totalFragments > 1000000) { /* 最多100万个分片 */
        LOG_ERROR("Invalid totalFragments: {}", totalFragments);
        return;
    }

    if (fragmentIndex >= totalFragments) {
        LOG_ERROR("Invalid fragmentIndex: {} >= {}", fragmentIndex, totalFragments);
        return;
    }

    /* 添加调试日志 */
    LOG_DEBUG("Fragment received - ID: {}, Index: {}/{}, Size: {}",
             messageId.toString(), fragmentIndex, totalFragments,
             ConvertUtil::formatFileSize(data.size()));

    /* 提取载荷 */
    rtc::binary fragment(data.begin() + HEADER_SIZE, data.end());

    QString fullMessageId = channelName + "_" + messageId.toString();
    reassembleFragment(fullMessageId, fragmentIndex, totalFragments, fragment);
}

void FilePacketUtil::reassembleFragment(const QString &messageId, quint64 fragmentIndex,
                                        quint64 totalFragments, const rtc::binary &fragment)
{
    QString completedTempFilePath;
    {
    QMutexLocker locker(&m_reassemblyMutex);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    cleanupExpiredReassembliesLocked(nowMs);

    if (totalFragments == 0 || fragmentIndex >= totalFragments)
    {
        LOG_ERROR("Invalid fragment parameters: index={}, total={}", fragmentIndex, totalFragments);
        return;
    }

    ReassemblyBuffer &buffer = m_reassemblyBuffers[messageId];

    /* 初始化缓冲区 */
    if (buffer.receivedFragments.empty())
    {
        buffer.totalFragments = totalFragments;
        buffer.receivedFragments.resize(totalFragments, false);
        buffer.timestamp = nowMs;

        /* 创建临时文件 */
        QString safeMessageId = QString(messageId).replace("/", "_").replace("\\", "_");
        buffer.tempFilePath = QDir::tempPath() + "/" + safeMessageId + ".tmp";
        buffer.tempFile = new QFile(buffer.tempFilePath);
        if (!buffer.tempFile->open(QIODevice::WriteOnly)) {
            LOG_ERROR("Failed to create temp file for reassembly: {}", buffer.tempFilePath);
            delete buffer.tempFile;
            buffer.tempFile = nullptr;
            m_reassemblyBuffers.erase(messageId);
            return;
        }

        LOG_DEBUG("Created reassembly temp file: {}", buffer.tempFilePath);
    }

    if (!buffer.tempFile) {
        LOG_ERROR("Temp file not available for fragment reassembly");
        return;
    }

    if (buffer.receivedFragments[fragmentIndex])
    {
        buffer.timestamp = nowMs;
        LOG_TRACE("Ignoring duplicate fragment {}/{} for {}", fragmentIndex + 1, totalFragments, messageId);
        return;
    }

    /* 验证偏移量计算的合理性 */
    quint64 offset = fragmentIndex * PAYLOAD_SIZE;

    if (offset > MAX_REASONABLE_OFFSET) {
        LOG_ERROR("Invalid fragment offset calculated: {} (fragmentIndex: {}, PAYLOAD_SIZE: {})",
                 offset, fragmentIndex, PAYLOAD_SIZE);
        return;
    }

    /* 将分片载荷写入文件的对应位置（按载荷偏移量，不是分片偏移量） */
    if (!buffer.tempFile->seek(offset)) {
        LOG_ERROR("Failed to seek temp file to offset: {}", offset);
        return;
    }

    qint64 written = buffer.tempFile->write(reinterpret_cast<const char*>(fragment.data()), fragment.size());

    if (written != static_cast<qint64>(fragment.size())) {
        LOG_ERROR("Failed to write fragment to temp file: {} (wanted: {}, written: {})",
                 buffer.tempFilePath, ConvertUtil::formatFileSize(fragment.size()), ConvertUtil::formatFileSize(written));
        return;
    }

    buffer.receivedFragments[fragmentIndex] = true;
    buffer.receivedIndexes.insert(fragmentIndex);
    buffer.receivedBytes += static_cast<quint64>(written);
    buffer.timestamp = nowMs;

    LOG_DEBUG("Fragment {}/{} written to temp file at offset {} ({})",
             fragmentIndex + 1, totalFragments, offset, ConvertUtil::formatFileSize(fragment.size()));

    /* 检查是否完整 */
    bool complete = (buffer.receivedIndexes.size() == totalFragments);

    if (complete)
    {
        LOG_DEBUG("Fragment reassembly complete, temp file: {}", buffer.tempFilePath);

        buffer.tempFile->close();
        delete buffer.tempFile;
        buffer.tempFile = nullptr;

        const qint64 expectedSize = static_cast<qint64>(buffer.receivedBytes);
        QFileInfo tempInfo(buffer.tempFilePath);
        if (tempInfo.exists() && expectedSize >= 0 && tempInfo.size() != expectedSize)
        {
            LOG_ERROR("Reassembled temp file size mismatch: {} got={}, expected={}",
                      buffer.tempFilePath, tempInfo.size(), expectedSize);
            QFile::remove(buffer.tempFilePath);
        }
        else
        {
            completedTempFilePath = buffer.tempFilePath;
        }

        m_reassemblyBuffers.erase(messageId);
    }
    }

    /* 直接处理临时文件，而不是读取到内存；处理过程可能落盘大文件，不能持有重组锁。 */
    if (!completedTempFilePath.isEmpty())
    {
        if (messageId.contains("file"))
            processFileDataPacket(completedTempFilePath);
        QFile::remove(completedTempFilePath);
    }
}
