#include "webrtc_cli.h"

#include "../common/constant.h"
#include "../util/convert_util.h"
#include "../util/file_packet_util.h"
#include "../util/json_util.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>

bool WebRtcCli::isTransferCancelled(const QString &transferId) const
{
    if (transferId.isEmpty())
        return false;
    QMutexLocker locker(&m_transferMutex);
    return m_cancelledTransfers.contains(transferId);
}

void WebRtcCli::markTransferCancelled(const QString &transferId)
{
    if (transferId.isEmpty())
        return;
    QMutexLocker locker(&m_transferMutex);
    m_cancelledTransfers.insert(transferId);
}

void WebRtcCli::clearTransferCancelled(const QString &transferId)
{
    if (transferId.isEmpty())
        return;
    QMutexLocker locker(&m_transferMutex);
    m_cancelledTransfers.remove(transferId);
}

void WebRtcCli::sendTransferProgress(const QString &transferId, qint64 transferredBytes, qint64 totalBytes,
                                     int transferredFiles, int totalFiles, const QString &ctlPath, const QString &cliPath)
{
    if (transferId.isEmpty())
        return;

    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_TRANSFER_PROGRESS)
                          .add(Constant::KEY_TRANSFER_ID, transferId)
                          .add(Constant::KEY_TRANSFER_BYTES, static_cast<double>(transferredBytes))
                          .add(Constant::KEY_TRANSFER_TOTAL_BYTES, static_cast<double>(totalBytes))
                          .add(Constant::KEY_TRANSFER_FILE_COUNT, transferredFiles)
                          .add(Constant::KEY_TRANSFER_TOTAL_FILES, totalFiles)
                          .build();
    if (!ctlPath.isEmpty())
        obj.insert(Constant::KEY_PATH_CTL, ctlPath);
    if (!cliPath.isEmpty())
        obj.insert(Constant::KEY_PATH_CLI, cliPath);
    sendFileTextChannelMessage(obj);
}

void WebRtcCli::sendTransferCancel(const QString &transferId)
{
    if (transferId.isEmpty())
        return;

    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_TRANSFER_CANCEL)
                          .add(Constant::KEY_TRANSFER_ID, transferId)
                          .build();
    sendFileTextChannelMessage(obj);
}

qint64 WebRtcCli::collectDirectoryStats(const QString &path, int *fileCount) const
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

void WebRtcCli::sendFile(const QString &cliPath, const QString &ctlPath, const QString &transferId)
{
    QFileInfo info(cliPath);
    if (!info.exists())
    {
        LOG_ERROR("File or directory does not exist: {}", cliPath);
        sendFileErrorResponse(cliPath, "File or directory does not exist");
        return;
    }

    if (info.isFile())
    {
        /* 发送单个文件 */
        sendSingleFile(cliPath, ctlPath, transferId, 0, info.size(), 1, 1);
    }
    else if (info.isDir())
    {
        /* 发送文件夹中的所有文件 */
        sendDirectory(cliPath, ctlPath, transferId);
    }
    else
    {
        LOG_ERROR("Unknown file type: {}", cliPath);
        sendFileErrorResponse(cliPath, "Unknown file type");
    }
}

bool WebRtcCli::sendSingleFile(const QString &cliPath, const QString &ctlPath, const QString &transferId,
                               qint64 baseBytes, qint64 totalBytes, int currentFileIndex, int totalFiles)
{
    QFileInfo fileInfo(cliPath);
    if (!fileInfo.exists() || !fileInfo.isFile())
    {
        LOG_ERROR("File does not exist or is not a regular file: {}", cliPath);
        sendFileErrorResponse(cliPath, "File does not exist or is not a regular file");
        return false;
    }

    QString absCtlPath = ctlPath;
    if (!absCtlPath.endsWith(fileInfo.fileName()))
    {
        absCtlPath = QDir::cleanPath(absCtlPath + "/" + fileInfo.fileName());
    }

    /* 创建包含文件信息的头部 */
    QJsonObject header = JsonUtil::createObject()
                             .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_DOWNLOAD)
                             .add(Constant::KEY_PATH_CLI, cliPath)
                             .add(Constant::KEY_PATH_CTL, absCtlPath)
                             .add(Constant::KEY_TRANSFER_ID, transferId)
                             .add(Constant::KEY_FILE_SIZE, static_cast<double>(fileInfo.size()))
                             .add(Constant::KEY_TRANSFER_TOTAL_BYTES, static_cast<double>(totalBytes >= 0 ? totalBytes : fileInfo.size()))
                             .add(Constant::KEY_TRANSFER_TOTAL_FILES, totalFiles)
                             .add("transferBaseBytes", static_cast<double>(baseBytes))
                             .add("transferFileIndex", currentFileIndex)
                             .add("isDirectory", false)
                             .build();

    /* 使用流式发送方法，避免将大文件加载到内存 */
    if (m_fileChannel && m_fileChannel->isOpen())
    {
        try
        {
            const qint64 effectiveTotalBytes = totalBytes >= 0 ? totalBytes : fileInfo.size();
            auto lastProgressMs = std::make_shared<qint64>(0);
            const auto progressCallback = [this, transferId, baseBytes, effectiveTotalBytes, currentFileIndex, totalFiles,
                                           fileSize = fileInfo.size(), lastProgressMs, absCtlPath, cliPath](qint64 sentBytes, qint64 packetTotalBytes) {
                const qint64 headerBytes = qMax<qint64>(0, packetTotalBytes - fileSize);
                const qint64 currentFileBytes = qBound<qint64>(0, sentBytes - headerBytes, fileSize);
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                if (currentFileBytes < fileSize && nowMs - *lastProgressMs < 120)
                    return;
                *lastProgressMs = nowMs;
                sendTransferProgress(transferId,
                                     qMin(baseBytes + currentFileBytes, effectiveTotalBytes),
                                     effectiveTotalBytes,
                                     currentFileBytes >= fileSize ? currentFileIndex : qMax(0, currentFileIndex - 1),
                                     totalFiles,
                                     absCtlPath,
                                     cliPath);
            };
            const auto cancelCallback = [this, transferId]() {
                return isTransferCancelled(transferId);
            };

            if (FilePacketUtil::sendFileStream(cliPath, header, m_fileChannel, progressCallback, cancelCallback))
            {
                LOG_INFO("Sent file stream: {} -> {} ({})",
                         cliPath, absCtlPath, ConvertUtil::formatFileSize(fileInfo.size()));
                return true;
            }
            else
            {
                LOG_ERROR("Failed to send file stream: {}", cliPath);
                if (!isTransferCancelled(transferId))
                    sendFileErrorResponse(cliPath, "Failed to send file stream");
                return false;
            }
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Exception during file stream send: {}", e.what());
            if (!isTransferCancelled(transferId))
                sendFileErrorResponse(cliPath, "Exception during file stream send");
            return false;
        }
        catch (...)
        {
            LOG_ERROR("Failed to send file stream: unknown error");
            if (!isTransferCancelled(transferId))
                sendFileErrorResponse(cliPath, "Exception during file stream send");
            return false;
        }
    }
    else
    {
        LOG_ERROR("File channel not available for sending file");
        sendFileErrorResponse(cliPath, "File channel not available");
        return false;
    }
}

bool WebRtcCli::sendFileMetadataPacket(const QJsonObject &header, const QString &transferId)
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

void WebRtcCli::sendDirectory(const QString &cliPath, const QString &ctlPath, const QString &transferId)
{
    QDir dir(cliPath);
    if (!dir.exists())
    {
        sendFileErrorResponse(cliPath, "Directory does not exist");
        return;
    }

    int totalFiles = 0;
    const qint64 totalBytes = collectDirectoryStats(cliPath, &totalFiles);
    sendTransferProgress(transferId, 0, totalBytes, 0, totalFiles, ctlPath, cliPath);

    /* 首先发送目录开始标记 */
    QJsonObject dirStartHeader = JsonUtil::createObject()
                                     .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_DOWNLOAD)
                                     .add(Constant::KEY_PATH_CLI, cliPath)
                                     .add(Constant::KEY_PATH_CTL, ctlPath)
                                     .add(Constant::KEY_TRANSFER_ID, transferId)
                                     .add(Constant::KEY_TRANSFER_TOTAL_BYTES, static_cast<double>(totalBytes))
                                     .add(Constant::KEY_TRANSFER_TOTAL_FILES, totalFiles)
                                     .add("isDirectory", true)
                                     .add("directoryStart", true)
                                     .build();

    if (!sendFileMetadataPacket(dirStartHeader, transferId))
    {
        sendFileErrorResponse(cliPath, "Failed to send directory metadata");
        return;
    }

    int fileCount = 0;
    qint64 transferredBytes = 0;
    bool hasErrors = false;
    QDirIterator it(cliPath,
                    QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        if (isTransferCancelled(transferId))
        {
            LOG_INFO("Download directory cancelled: {}", cliPath);
            return;
        }

        const QString filePath = it.next();
        QFileInfo fileInfo(filePath);
        QString relativePath = dir.relativeFilePath(fileInfo.absoluteFilePath());
        QString fullRemotePath = QDir::cleanPath(ctlPath + "/" + relativePath);
        if (sendSingleFile(fileInfo.absoluteFilePath(), fullRemotePath, transferId,
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

    /* 发送目录结束标记 */
    QJsonObject dirEndHeader = JsonUtil::createObject()
                                   .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_DOWNLOAD)
                                   .add(Constant::KEY_PATH_CLI, cliPath)
                                   .add(Constant::KEY_PATH_CTL, ctlPath)
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

    LOG_INFO("Sent directory: {} -> {} ({} files, hasErrors={})", cliPath, ctlPath, fileCount, hasErrors);
    sendTransferProgress(transferId, totalBytes, totalBytes, totalFiles, totalFiles, ctlPath, cliPath);
}

void WebRtcCli::sendFileErrorResponse(const QString &filePath, const QString &error)
{
    QJsonObject errorMsg = JsonUtil::createObject()
                               .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                               .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_DOWNLOAD)
                               .add(Constant::KEY_PATH, filePath)
                               .add(Constant::KEY_PATH_CLI, filePath)
                               .add(Constant::KEY_ERROR, error)
                               .build();

    sendFileTextChannelMessage(errorMsg);
}

void WebRtcCli::sendUploadResponse(const QString &fileName, bool success, const QString &message)
{
    QJsonObject responseMsg = JsonUtil::createObject()
                                  .add(Constant::KEY_MSGTYPE, Constant::TYPE_UPLOAD_FILE_RES)
                                  .add(Constant::KEY_PATH_CLI, fileName)
                                  .add("status", success)
                                  .add("message", message)
                                  .build();

    sendFileTextChannelMessage(responseMsg);
}

void WebRtcCli::handleFileReceived(bool status, const QString &tempPath)
{
    LOG_INFO("Received complete file from FilePacketUtil, status: {}, tempPath: {}", status, tempPath);

    sendUploadResponse(tempPath, status, status ? "Upload successful" : "Upload failed");
}

void WebRtcCli::saveUploadedFile(const QString &filePath, const QByteArray &data)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
    {
        LOG_ERROR("Failed to open file for writing: {}", filePath);

        /* 发送错误响应 */
        QJsonObject errorMsg = JsonUtil::createObject()
                                   .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                                   .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_UPLOAD)
                                   .add(Constant::KEY_PATH, filePath)
                                   .add("error", "Failed to save file")
                                   .build();

        sendFileTextChannelMessage(errorMsg);
        return;
    }

    file.write(data);
    file.close();

    /* 发送成功响应 */
    QJsonObject successMsg = JsonUtil::createObject()
                                 .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                                 .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_UPLOAD)
                                 .add(Constant::KEY_PATH, filePath)
                                 .add("success", true)
                                 .add("size", data.size())
                                 .build();

    sendFileTextChannelMessage(successMsg);
    LOG_INFO("Saved uploaded file: {} ({})", filePath, ConvertUtil::formatFileSize(data.size()));
}
