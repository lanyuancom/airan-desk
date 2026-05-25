/* Split from file_packet_util.cpp by packet transfer responsibility. */

#include "file_packet_util.h"
#include "config_util.h"
#include "json_util.h"

#include <QApplication>
#include <QMessageBox>
#include <QMetaObject>

namespace
{
constexpr size_t kFileChannelHighWatermark = 2 * 1024 * 1024;
constexpr size_t kFileChannelLowWatermark = 512 * 1024;
constexpr int kBackpressureSleepMs = 5;
constexpr int kBackpressureTimeoutMs = 30000;
constexpr qint64 kReassemblyTimeoutMs = 10 * 60 * 1000;

/* Helper QObject to run dialogs on the GUI thread for Qt versions that */
/* don't support functor overloads of QMetaObject::invokeMethod. */
class AskInvoker : public QObject
{
    Q_OBJECT
public:
    explicit AskInvoker(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    void ask(const QString &path, bool *outAllow)
    {
        const QMessageBox::StandardButton result = QMessageBox::question(
            nullptr,
            QCoreApplication::translate("FilePacketUtil", "文件已存在"),
            QCoreApplication::translate("FilePacketUtil", "目标文件已存在：\n%1\n\n是否覆盖？").arg(QDir::toNativeSeparators(path)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes);
        if (outAllow)
            *outAllow = (result == QMessageBox::Yes);
    }
};

bool confirmOverwriteIfNeeded(const QString &targetPath)
{
    if (!QFileInfo::exists(targetPath) || !ConfigUtil->showUI)
        return true;

    QApplication *app = qobject_cast<QApplication *>(QApplication::instance());
    if (!app)
        return true;

    bool allowOverwrite = true;

    if (QThread::currentThread() == app->thread()) {
        /* We're on GUI thread already, run directly. */
        const QMessageBox::StandardButton result = QMessageBox::question(
            nullptr,
            QCoreApplication::translate("FilePacketUtil", "文件已存在"),
            QCoreApplication::translate("FilePacketUtil", "目标文件已存在：\n%1\n\n是否覆盖？").arg(QDir::toNativeSeparators(targetPath)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes);
        allowOverwrite = (result == QMessageBox::Yes);
    } else {
        /* Ensure a single AskInvoker instance lives on the application (GUI) thread. */
        static AskInvoker *invoker = nullptr;
        static QMutex invokerMutex;
        if (!invoker)
        {
            QMutexLocker locker(&invokerMutex);
            if (!invoker)
            {
                invoker = new AskInvoker(app);
                invoker->moveToThread(app->thread());
            }
        }
        QMetaObject::invokeMethod(invoker, "ask", Qt::BlockingQueuedConnection,
                                  Q_ARG(QString, targetPath), Q_ARG(bool *, &allowOverwrite));
    }

    return allowOverwrite;
}
}


void FilePacketUtil::processFileDataPacket(const QString &tempFilePath)
{
    QFile tempFile(tempFilePath);
    if (!tempFile.open(QIODevice::ReadOnly)) {
        LOG_ERROR("Failed to open temp file for processing: {}", tempFilePath);
        return;
    }

    /* 只读取头部信息（前4个字节 + 头部JSON） */
    if (tempFile.size() < 4) {
        LOG_ERROR("Temp file too small to contain header size");
        tempFile.close();
        return;
    }

    QByteArray headerSizeBytes = tempFile.read(4);
    QDataStream stream(headerSizeBytes);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 headerSize;
    stream >> headerSize;

    if (headerSize > tempFile.size() - 4) {
        LOG_ERROR("Invalid header size: {}, total file: {}", headerSize, tempFile.size());
        tempFile.close();
        return;
    }

    /* 读取头部JSON */
    QByteArray headerBytes = tempFile.read(headerSize);
    QJsonObject header = JsonUtil::safeParseObject(headerBytes);

    if (!JsonUtil::isValidObject(header)) {
        LOG_ERROR("Failed to parse file data packet header");
        tempFile.close();
        return;
    }

    QString msgType = JsonUtil::getString(header, Constant::KEY_MSGTYPE);
    QString ctlPath = JsonUtil::getString(header, Constant::KEY_PATH_CTL);
    QString cliPath = JsonUtil::getString(header, Constant::KEY_PATH_CLI);
    const QString transferId = JsonUtil::getString(header, Constant::KEY_TRANSFER_ID);
    const bool isDirectory = JsonUtil::getBool(header, "isDirectory", false);
    const bool directoryStart = JsonUtil::getBool(header, "directoryStart", false);
    const bool directoryEnd = JsonUtil::getBool(header, "directoryEnd", false);
    const bool transferStatus = JsonUtil::getBool(header, "status", true);

    {
        QMutexLocker locker(&m_reassemblyMutex);
        if (!transferId.isEmpty() && m_cancelledTransfers.contains(transferId))
        {
            LOG_INFO("Drop cancelled file packet: transferId={}, cliPath={}, ctlPath={}",
                     transferId, cliPath, ctlPath);
            tempFile.close();
            return;
        }
    }

    /* 计算实际文件数据的大小和起始位置 */
    qint64 fileDataStart = 4 + headerSize;
    qint64 fileDataSize = tempFile.size() - fileDataStart;

    if (isDirectory)
    {
        const QString targetPath = (msgType == Constant::TYPE_FILE_DOWNLOAD) ? ctlPath : cliPath;
        if (!targetPath.isEmpty() && directoryStart)
        {
            if (!QDir().mkpath(targetPath))
            {
                LOG_ERROR("Failed to create target directory: {}", targetPath);
                if (msgType == Constant::TYPE_FILE_DOWNLOAD)
                    emit fileDownloadCompleted(false, targetPath);
                else if (msgType == Constant::TYPE_FILE_UPLOAD)
                    emit fileReceived(false, targetPath);
            }
            else
            {
                LOG_INFO("Created target directory: {}", targetPath);
            }
        }
        if (!targetPath.isEmpty() && directoryEnd)
        {
            if (msgType == Constant::TYPE_FILE_DOWNLOAD)
                emit fileDownloadCompleted(transferStatus, targetPath);
            else if (msgType == Constant::TYPE_FILE_UPLOAD)
                emit fileReceived(transferStatus, targetPath);
        }
    }
    else if (msgType == Constant::TYPE_FILE_DOWNLOAD && !ctlPath.isEmpty() && !cliPath.isEmpty())
    {
        /* 流式复制文件数据到目标位置 */
        if (streamCopyFile(tempFile, fileDataStart, ctlPath, fileDataSize)) {
            emit fileDownloadCompleted(true, ctlPath);
            LOG_INFO("Received file download: {} ({})", ctlPath, ConvertUtil::formatFileSize(fileDataSize));
        } else {
            emit fileDownloadCompleted(false, ctlPath);
        }
    }
    else if (msgType == Constant::TYPE_FILE_UPLOAD && !ctlPath.isEmpty() && !cliPath.isEmpty())
    {
        /* 流式复制文件数据到目标位置 */
        if (streamCopyFile(tempFile, fileDataStart, cliPath, fileDataSize)) {
            emit fileReceived(true, cliPath);
            LOG_INFO("Received file upload: {} ({})", cliPath, ConvertUtil::formatFileSize(fileDataSize));
        } else {
            emit fileReceived(false, cliPath);
        }
    }
    else
    {
        LOG_WARNING("Unknown file data packet type: {}, headerSize={} bytes", msgType, headerBytes.size());
    }

    tempFile.close();
}

void FilePacketUtil::cancelTransfer(const QString &transferId)
{
    if (transferId.isEmpty())
        return;

    QMutexLocker locker(&m_reassemblyMutex);
    m_cancelledTransfers.insert(transferId);
}

void FilePacketUtil::cancelAllReassemblies()
{
    QMutexLocker locker(&m_reassemblyMutex);
    for (auto &pair : m_reassemblyBuffers)
    {
        if (pair.second.tempFile)
        {
            pair.second.tempFile->close();
            delete pair.second.tempFile;
            pair.second.tempFile = nullptr;
        }
        if (!pair.second.tempFilePath.isEmpty())
            QFile::remove(pair.second.tempFilePath);
    }
    m_reassemblyBuffers.clear();
}

bool FilePacketUtil::streamCopyFile(QFile &sourceFile, qint64 sourceOffset, const QString &targetPath, qint64 dataSize)
{
    /* 确保目标目录存在 */
    QFileInfo targetFileInfo(targetPath);
    QDir().mkpath(targetFileInfo.absolutePath());

    if (!confirmOverwriteIfNeeded(targetPath))
    {
        LOG_INFO("User declined to overwrite target file: {}", targetPath);
        return false;
    }

    /* 打开目标文件 */
    QFile targetFile(targetPath);
    if (!targetFile.open(QIODevice::WriteOnly)) {
        LOG_ERROR("Failed to create target file: {} error: {}", targetPath, targetFile.errorString());
        return false;
    }

    /* 定位到源文件的数据起始位置 */
    if (!sourceFile.seek(sourceOffset)) {
        LOG_ERROR("Failed to seek source file to offset: {}", sourceOffset);
        targetFile.close();
        return false;
    }

    /* 流式复制数据 */
    const qint64 BUFFER_SIZE = 64 * 1024; /* 64KB 缓冲区 */
    QByteArray buffer;
    qint64 totalCopied = 0;
    qint64 remaining = dataSize;

    while (remaining > 0 && !sourceFile.atEnd()) {
        qint64 toRead = qMin(BUFFER_SIZE, remaining);
        buffer = sourceFile.read(toRead);

        if (buffer.isEmpty()) {
            break;
        }

        qint64 written = targetFile.write(buffer);
        if (written != buffer.size()) {
            LOG_ERROR("Failed to write to target file: {} written: {}, expected: {}",
                     targetPath, written, buffer.size());
            targetFile.close();
            QFile::remove(targetPath);
            return false;
        }

        totalCopied += written;
        remaining -= written;
    }

    targetFile.flush();
    targetFile.close();

    /* 验证复制的数据大小 */
    if (totalCopied != dataSize) {
        LOG_ERROR("File copy size mismatch: {} copied: {}, expected: {}",
                 targetPath, totalCopied, dataSize);
        QFile::remove(targetPath);
        return false;
    }

    /* 等待文件真正落盘 */
    QFileInfo checkFile(targetPath);
    checkFile.refresh();
    int retries = 0;
    while (!checkFile.exists() && retries < 10) {
        QThread::msleep(10);
        checkFile.refresh();
        retries++;
    }

    if (!checkFile.exists()) {
        LOG_ERROR("Target file does not exist after copy: {}", targetPath);
        return false;
    }

    return true;
}


#include "file_packet_util_file.moc"
