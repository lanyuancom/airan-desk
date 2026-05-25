#include "webrtc_ctl.h"

#include "../common/constant.h"
#include "../util/convert_util.h"
#include "../util/file_packet_util.h"
#include "../util/json_util.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
bool WebRtcCtl::isTransferCancelled(const QString &transferId) const
{
    if (transferId.isEmpty())
        return false;
    QMutexLocker locker(&m_transferMutex);
    return m_cancelledTransfers.contains(transferId);
}

void WebRtcCtl::markTransferCancelled(const QString &transferId)
{
    if (transferId.isEmpty())
        return;
    QMutexLocker locker(&m_transferMutex);
    m_cancelledTransfers.insert(transferId);
}

void WebRtcCtl::clearTransferCancelled(const QString &transferId)
{
    if (transferId.isEmpty())
        return;
    QMutexLocker locker(&m_transferMutex);
    m_cancelledTransfers.remove(transferId);
}

void WebRtcCtl::sendTransferCancel(const QString &transferId)
{
    if (transferId.isEmpty() || !m_fileTextChannel || !m_fileTextChannel->isOpen())
        return;

    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_TRANSFER_CANCEL)
                          .add(Constant::KEY_TRANSFER_ID, transferId)
                          .build();
    try
    {
        m_fileTextChannel->send(rtc::message_variant(JsonUtil::toCompactBytes(obj).toStdString()));
    }
    catch (const std::exception &e)
    {
        LOG_WARN("Failed to send transfer cancel: {}", e.what());
    }
}

void WebRtcCtl::cancelFileTransfer(const QString &transferId)
{
    markTransferCancelled(transferId);
    if (m_filePacketUtil)
    {
        m_filePacketUtil->cancelTransfer(transferId);
        m_filePacketUtil->cancelAllReassemblies();
    }
    sendTransferCancel(transferId);
    LOG_INFO("Cancel requested for file transfer: {}", transferId);
}

void WebRtcCtl::emitTransferProgress(const QString &transferId, qint64 transferredBytes, qint64 totalBytes,
                                     int transferredFiles, int totalFiles)
{
    if (transferId.isEmpty())
        return;
    emit fileTransferProgress(transferId, transferredBytes, totalBytes, transferredFiles, totalFiles);
}

qint64 WebRtcCtl::collectDirectoryStats(const QString &path, int *fileCount) const
{
    if (fileCount)
        *fileCount = 0;

    QFileInfo info(path);
    if (!info.exists())
        return 0;

    if (info.isFile())
    {
        if (fileCount)
            *fileCount = 1;
        return info.size();
    }

    qint64 totalBytes = 0;
    int totalFiles = 0;
    QDirIterator it(path,
                    QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        QFileInfo fileInfo(it.next());
        totalBytes += fileInfo.size();
        ++totalFiles;
    }
    if (fileCount)
        *fileCount = totalFiles;
    return totalBytes;
}

void WebRtcCtl::uploadFile2CLI(const QString &ctlPath, const QString &cliPath, const QString &transferId)
{
    LOG_WARN("uploadFile2CLI called: {} -> {}", ctlPath, cliPath);
    clearTransferCancelled(transferId);

    if (!m_fileChannel || !m_fileChannel->isOpen())
    {
        LOG_ERROR("File channel not available");
        emit recvUploadFileRes(false, ctlPath);
        return;
    }

    /* 构造完整的本地文件路径 */
    QString fullLocalPath = ctlPath;

    QFileInfo fileInfo(ctlPath);
    if (!fileInfo.exists())
    {
        LOG_ERROR("File does not exist: {}", ctlPath);
        emit recvUploadFileRes(false, ctlPath);
        return;
    }

    if (fileInfo.isFile())
    {
        /* 上传单个文件 */
        uploadSingleFile(ctlPath, cliPath, transferId, 0, fileInfo.size(), 1, 1);
    }
    else if (fileInfo.isDir())
    {
        /* 上传目录 */
        uploadDirectory(ctlPath, cliPath, transferId);
    }
    else
    {
        LOG_ERROR("Unknown file type: {}", ctlPath);
        emit recvUploadFileRes(false, ctlPath);
    }
}

bool WebRtcCtl::uploadSingleFile(const QString &ctlPath, const QString &cliPath, const QString &transferId,
                                 qint64 baseBytes, qint64 totalBytes, int currentFileIndex, int totalFiles)
{
    QFileInfo fileInfo(ctlPath);
    if (!fileInfo.exists() || !fileInfo.isFile())
    {
        LOG_ERROR("File does not exist or is not a regular file: {}", ctlPath);
        emit recvUploadFileRes(false, cliPath);
        return false;
    }

    /* 创建包含文件信息的头部 */
    QJsonObject header = JsonUtil::createObject()
                             .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_UPLOAD)
                             .add(Constant::KEY_PATH_CTL, ctlPath)
                             .add(Constant::KEY_PATH_CLI, cliPath)
                             .add(Constant::KEY_TRANSFER_ID, transferId)
                             .add(Constant::KEY_FILE_SIZE, static_cast<double>(fileInfo.size()))
                             .add("isDirectory", false)
                             .build();

    /* 使用流式发送方法，避免将大文件加载到内存 */
    if (m_fileChannel && m_fileChannel->isOpen())
    {
        try
        {
            const qint64 effectiveTotalBytes = totalBytes >= 0 ? totalBytes : fileInfo.size();
            auto lastProgressMs = std::make_shared<qint64>(0);
            const auto progressCallback = [this, transferId, baseBytes, effectiveTotalBytes, currentFileIndex, totalFiles, fileSize = fileInfo.size(), lastProgressMs](qint64 sentBytes, qint64 packetTotalBytes) {
                const qint64 headerBytes = qMax<qint64>(0, packetTotalBytes - fileSize);
                const qint64 currentFileBytes = qBound<qint64>(0, sentBytes - headerBytes, fileSize);
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                if (currentFileBytes < fileSize && nowMs - *lastProgressMs < 120)
                    return;
                *lastProgressMs = nowMs;
                emitTransferProgress(transferId,
                                     qMin(baseBytes + currentFileBytes, effectiveTotalBytes),
                                     effectiveTotalBytes,
                                     currentFileBytes >= fileSize ? currentFileIndex : qMax(0, currentFileIndex - 1),
                                     totalFiles);
            };
            const auto cancelCallback = [this, transferId]() {
                return isTransferCancelled(transferId);
            };

            if (FilePacketUtil::sendFileStream(ctlPath, header, m_fileChannel, progressCallback, cancelCallback))
            {
                LOG_INFO("Sent file stream: {} -> {} ({})",
                         ctlPath, cliPath, ConvertUtil::formatFileSize(fileInfo.size()));
                return true;
            }
            else
            {
                LOG_ERROR("Failed to send file stream: {}", ctlPath);
                if (!isTransferCancelled(transferId))
                    emit recvUploadFileRes(false, cliPath);
                return false;
            }
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Exception during file stream send: {}", e.what());
            if (!isTransferCancelled(transferId))
                emit recvUploadFileRes(false, cliPath);
            return false;
        }
        catch (...)
        {
            LOG_ERROR("Failed to send file stream: unknown error");
            if (!isTransferCancelled(transferId))
                emit recvUploadFileRes(false, cliPath);
            return false;
        }
    }
    else
    {
        LOG_ERROR("File channel not available for uploading file");
        emit recvUploadFileRes(false, cliPath);
        return false;
    }
}

bool WebRtcCtl::sendFileMetadataPacket(const QJsonObject &header, const QString &transferId)
{
    if (!m_fileChannel || !m_fileChannel->isOpen())
    {
        LOG_ERROR("File channel not available for metadata packet");
        return false;
    }
    try
    {
        const auto cancelCallback = [this, transferId]() {
            return isTransferCancelled(transferId);
        };
        return FilePacketUtil::sendDataPacket(header, QByteArray(), m_fileChannel, FilePacketUtil::ProgressCallback(), cancelCallback);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Exception during metadata packet send: {}", e.what());
    }
    catch (...)
    {
        LOG_ERROR("Exception during metadata packet send: unknown error");
    }
    return false;
}

void WebRtcCtl::uploadDirectory(const QString &ctlPath, const QString &cliPath, const QString &transferId)
{
    QDir dir(ctlPath);
    if (!dir.exists())
    {
        LOG_ERROR("Directory does not exist: {}", ctlPath);
        emit recvUploadFileRes(false, cliPath);
        return;
    }

    int totalFiles = 0;
    const qint64 totalBytes = collectDirectoryStats(ctlPath, &totalFiles);
    emitTransferProgress(transferId, 0, totalBytes, 0, totalFiles);

    /* 首先发送目录开始标记 */
    QJsonObject dirStartHeader = JsonUtil::createObject()
                                     .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_UPLOAD)
                                     .add(Constant::KEY_PATH_CTL, ctlPath)
                                     .add(Constant::KEY_PATH_CLI, cliPath)
                                     .add(Constant::KEY_TRANSFER_ID, transferId)
                                     .add(Constant::KEY_TRANSFER_TOTAL_BYTES, static_cast<double>(totalBytes))
                                     .add(Constant::KEY_TRANSFER_TOTAL_FILES, totalFiles)
                                     .add("isDirectory", true)
                                     .add("directoryStart", true)
                                     .build();
    if (!sendFileMetadataPacket(dirStartHeader, transferId))
    {
        emit recvUploadFileRes(false, cliPath);
        return;
    }

    int fileCount = 0;
    qint64 transferredBytes = 0;
    bool hasErrors = false;

    QDirIterator it(ctlPath,
                    QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        if (isTransferCancelled(transferId))
        {
            LOG_INFO("Upload directory cancelled: {}", ctlPath);
            return;
        }

        const QString filePath = it.next();
        QFileInfo fileInfo(filePath);
        QString relativePath = dir.relativeFilePath(fileInfo.absoluteFilePath());
        QString fullRemotePath = QDir::cleanPath(cliPath + "/" + relativePath);
        if (uploadSingleFile(fileInfo.absoluteFilePath(), fullRemotePath, transferId,
                             transferredBytes, totalBytes, fileCount + 1, totalFiles))
        {
            fileCount++;
            transferredBytes += fileInfo.size();
        }
        else
        {
            if (isTransferCancelled(transferId))
                return;
            hasErrors = true;
        }
    }

    if (fileCount == 0)
    {
        LOG_INFO("Directory has no files, sent directory metadata only: {}", ctlPath);
    }
    else
    {
        LOG_INFO("Uploaded directory: {} -> {} ({} files, hasErrors={})", ctlPath, cliPath, fileCount, hasErrors);
    }
    /* 发送目录结束标记 */
    QJsonObject dirEndHeader = JsonUtil::createObject()
                                   .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_UPLOAD)
                                   .add(Constant::KEY_PATH_CTL, ctlPath)
                                   .add(Constant::KEY_PATH_CLI, cliPath)
                                   .add(Constant::KEY_TRANSFER_ID, transferId)
                                   .add(Constant::KEY_TRANSFER_TOTAL_BYTES, static_cast<double>(totalBytes))
                                   .add(Constant::KEY_TRANSFER_TOTAL_FILES, totalFiles)
                                   .add("isDirectory", true)
                                   .add("directoryEnd", true)
                                   .add("fileCount", fileCount)
                                   .add("status", !hasErrors)
                                   .build();
    if (!sendFileMetadataPacket(dirEndHeader, transferId))
        hasErrors = true;

    LOG_INFO("Sent directory: {} -> {} ({} files)", ctlPath, cliPath, fileCount);
    emitTransferProgress(transferId, totalBytes, totalBytes, totalFiles, totalFiles);
    emit recvUploadFileRes(!hasErrors, cliPath);
}

