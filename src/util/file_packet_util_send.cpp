/* Split from file_packet_util.cpp by packet transfer responsibility. */

#include "file_packet_util.h"
#include "json_util.h"

#include <chrono>

namespace
{
constexpr size_t kFileChannelHighWatermark = 2 * 1024 * 1024;
constexpr size_t kFileChannelLowWatermark = 512 * 1024;
constexpr int kBackpressureSleepMs = 5;
constexpr int kBackpressureTimeoutMs = 30000;
}

bool FilePacketUtil::waitForChannelBackpressure(const std::shared_ptr<rtc::DataChannel> &channel, const QString &filePath,
                                                const CancelCallback &cancelCallback)
{
    if (cancelCallback && cancelCallback())
    {
        LOG_INFO("File channel send cancelled before backpressure wait: {}", filePath);
        return false;
    }

    if (!channel || !channel->isOpen())
    {
        LOG_ERROR("File channel closed while streaming: {}", filePath);
        return false;
    }

    if (channel->bufferedAmount() < kFileChannelHighWatermark)
        return true;

    const auto start = std::chrono::steady_clock::now();
    while (channel && channel->isOpen() && channel->bufferedAmount() > kFileChannelLowWatermark)
    {
        if (cancelCallback && cancelCallback())
        {
            LOG_INFO("File channel send cancelled while waiting for backpressure: {}", filePath);
            return false;
        }

        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
        if (elapsedMs > kBackpressureTimeoutMs)
        {
            LOG_ERROR("Timed out waiting for file channel backpressure to drain: {}, buffered={} bytes",
                      filePath,
                      channel->bufferedAmount());
            return false;
        }
        QThread::msleep(kBackpressureSleepMs);
    }

    if (!channel || !channel->isOpen())
    {
        LOG_ERROR("File channel closed while waiting for backpressure: {}", filePath);
        return false;
    }
    return true;
}

bool FilePacketUtil::sendFileStream(const QString &filePath, const QJsonObject &header, std::shared_ptr<rtc::DataChannel> channel,
                                    const ProgressCallback &progressCallback, const CancelCallback &cancelCallback)
{
    if (!channel || !channel->isOpen()) {
        LOG_ERROR("Channel not available for file streaming");
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        LOG_ERROR("Failed to open file for streaming: {} error: {}", filePath, file.errorString());
        return false;
    }

    return sendPacketStream(&file, QByteArray(), header, channel, filePath, progressCallback, cancelCallback);
}

bool FilePacketUtil::sendDataPacket(const QJsonObject &header, const QByteArray &payload, std::shared_ptr<rtc::DataChannel> channel,
                                    const ProgressCallback &progressCallback, const CancelCallback &cancelCallback)
{
    return sendPacketStream(nullptr, payload, header, channel, JsonUtil::getString(header, Constant::KEY_PATH_CLI, QStringLiteral("metadata")),
                            progressCallback, cancelCallback);
}

bool FilePacketUtil::sendPacketStream(QFile *file, const QByteArray &payload, const QJsonObject &header,
                                      std::shared_ptr<rtc::DataChannel> channel, const QString &logPath,
                                      const ProgressCallback &progressCallback, const CancelCallback &cancelCallback)
{
    if (!channel || !channel->isOpen()) {
        LOG_ERROR("Channel not available for packet streaming");
        return false;
    }

    /* 准备头部数据 */
    QByteArray headerBytes = JsonUtil::toCompactBytes(header);
    QByteArray headerSizeBytes;
    QDataStream headerStream(&headerSizeBytes, QIODevice::WriteOnly);
    headerStream.setByteOrder(QDataStream::BigEndian);
    headerStream << static_cast<quint32>(headerBytes.size());

    /* 计算总数据大小 */
    quint64 totalDataSize = 4 + headerBytes.size() + payload.size();
    if (file)
        totalDataSize += file->size();
    quint64 totalFragments = (totalDataSize + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;
    if (totalFragments == 0)
        totalFragments = 1;

    LOG_INFO("Starting stream send for packet: {} ({}, {} fragments)", logPath,
             ConvertUtil::formatFileSize(totalDataSize), totalFragments);

    /* 生成消息ID */
    QUuid messageId = QUuid::createUuid();
    QByteArray messageIdBytes = messageId.toRfc4122();

    LOG_DEBUG("Generated message ID: {}", messageId.toString());

    /* 准备数据源缓冲区，先加入头部数据 */
    QByteArray dataBuffer;
    dataBuffer.append(headerSizeBytes);
    dataBuffer.append(headerBytes);
    dataBuffer.append(payload);

    quint64 fragmentIndex = 0;
    quint64 totalSent = 0;

    /* 开始流式发送分包 */
    while (fragmentIndex < totalFragments) {
        if (cancelCallback && cancelCallback())
        {
            LOG_INFO("Packet stream send cancelled: {}", logPath);
            if (file)
                file->close();
            return false;
        }

        /* 准备当前分包数据 */
        QByteArray fragmentPayload;

        /* 先从缓冲区获取数据 */
        if (!dataBuffer.isEmpty()) {
            int toTake = qMin(static_cast<int>(PAYLOAD_SIZE), dataBuffer.size());
            fragmentPayload.append(dataBuffer.left(toTake));
            dataBuffer.remove(0, toTake);
        }

        /* 如果载荷还不够且文件还有数据，从文件读取 */
        while (file && fragmentPayload.size() < PAYLOAD_SIZE && !file->atEnd()) {
            QByteArray fileData = file->read(PAYLOAD_SIZE - fragmentPayload.size());
            if (fileData.isEmpty()) {
                break;
            }
            fragmentPayload.append(fileData);
        }

        /* 如果没有数据了就结束 */
        if (fragmentPayload.isEmpty()) {
            break;
        }

        /* 创建完整的分包 */
        rtc::binary fragment(HEADER_SIZE + static_cast<quint64>(fragmentPayload.size()));

        /* 写入分包头部 */
        std::memcpy(fragment.data(), messageIdBytes.constData(), 16);

        /* 写入总分包数（大端序） */
        for (int i = 0; i < 8; ++i) {
            int shift = (7-i) * 8;
            uint64_t totalFragments64 = static_cast<uint64_t>(totalFragments);
            uint64_t shifted = totalFragments64 >> shift;
            std::byte val = static_cast<std::byte>(shifted & 0xFF);
            fragment[16 + i] = val;
        }

        /* 写入分包索引（大端序） */
        for (int i = 0; i < 8; ++i) {
            int shift = (7-i) * 8;
            uint64_t fragmentIndex64 = static_cast<uint64_t>(fragmentIndex);
            uint64_t shifted = fragmentIndex64 >> shift;
            std::byte val = static_cast<std::byte>(shifted & 0xFF);
            fragment[24 + i] = val;
        }

        /* 复制载荷数据 */
        std::memcpy(fragment.data() + HEADER_SIZE, fragmentPayload.constData(), fragmentPayload.size());

        /* 发送分包 */
        try {
            if (!waitForChannelBackpressure(channel, logPath, cancelCallback))
            {
                if (file)
                    file->close();
                return false;
            }
            if (!channel->send(fragment))
            {
                LOG_TRACE("File fragment buffered by SCTP: index={}, buffered={} bytes",
                          fragmentIndex,
                          channel->bufferedAmount());
            }
            totalSent += fragmentPayload.size();
            if (progressCallback)
                progressCallback(static_cast<qint64>(totalSent), static_cast<qint64>(totalDataSize));

            /* 定期输出进度日志，并包含更多调试信息 */
            if (fragmentIndex % 100 == 0 || fragmentIndex == totalFragments - 1) {
                LOG_DEBUG("Sent fragment {}/{} ({}) - MessageID: {}",
                         fragmentIndex + 1, totalFragments, ConvertUtil::formatFileSize(totalSent),
                         messageId.toString());
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to send fragment {}: {}", fragmentIndex, e.what());
            if (file)
                file->close();
            return false;
        }

        fragmentIndex++;
    }

    if (file)
        file->close();

    if (fragmentIndex != totalFragments || totalSent != totalDataSize)
    {
        LOG_ERROR("Packet stream send incomplete: {} sent={} expected={} fragments={}/{}",
                  logPath, ConvertUtil::formatFileSize(totalSent), ConvertUtil::formatFileSize(totalDataSize),
                  fragmentIndex, totalFragments);
        return false;
    }

    LOG_INFO("Successfully sent packet stream: {} ({}, {} fragments)",
             logPath, ConvertUtil::formatFileSize(totalDataSize), totalFragments);
    return true;
}

