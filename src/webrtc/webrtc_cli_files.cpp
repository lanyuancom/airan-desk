/* Split from webrtc_cli.cpp by client-side responsibility. */

#include "webrtc_cli.h"
#include "../common/constant.h"
#include "../util/file_packet_util.h"
#include "../util/json_util.h"

#include <QStorageInfo>

void WebRtcCli::populateLocalFiles()
{
    /* 获取已挂载的驱动器路径 */
    QJsonArray mountedPaths;
    QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &volume : volumes)
    {
        if (volume.isValid() && volume.isReady())
        {
            mountedPaths.append(volume.rootPath());
        }
    }

    m_currentDir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    m_currentDir.setSorting(QDir::Name | QDir::DirsFirst);

    QFileInfoList list = m_currentDir.entryInfoList();

    QJsonArray fileArray;
    for (const QFileInfo &entry : list)
    {
        QJsonObject fileObj = JsonUtil::createObject()
                                  .add(Constant::KEY_NAME, entry.fileName())
                                  .add(Constant::KEY_IS_DIR, entry.isDir())
                                  .add(Constant::KEY_FILE_SIZE, static_cast<double>(entry.size()))
                                  .add(Constant::KEY_FILE_SUFFIX, entry.isFile() ? entry.suffix().toLower() : QString())
                                  .add(Constant::KEY_FILE_EXECUTABLE, entry.isFile() && entry.isExecutable())
                                  .add(Constant::KEY_FILE_LAST_MOD_TIME, entry.lastModified().toString(Qt::ISODate))
                                  .build();
        fileArray.append(fileObj);
    }

    QJsonObject responseMsg = JsonUtil::createObject()
                                  .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                                  .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_LIST)
                                  .add(Constant::KEY_PATH, m_currentDir.absolutePath())
                                  .add(Constant::KEY_FOLDER_FILES, fileArray)
                                  .add(Constant::KEY_FOLDER_MOUNTED, mountedPaths)
                                  .build();

    sendFileTextChannelMessage(responseMsg);
}

void WebRtcCli::parseFileMsg(const QJsonObject &object)
{
    if (QThread::currentThread() != thread())
    {
        QMetaObject::invokeMethod(this, "parseFileMsg",
                                  Qt::QueuedConnection,
                                  Q_ARG(QJsonObject, object));
        return;
    }

    QString msgType = JsonUtil::getString(object, Constant::KEY_MSGTYPE);
    if (msgType.isEmpty())
    {
        LOG_ERROR("parseFileMsg: Missing msgType");
        return;
    }

    if (msgType == Constant::TYPE_FILE_LIST)
    {
        QString path = JsonUtil::getString(object, Constant::KEY_PATH);
        LOG_INFO("Processing file list request for path: {}", path);
        if (path.isEmpty())
        {
            LOG_ERROR("parseFileMsg: Missing path for file list request");
            return;
        }
        if (path == Constant::FOLDER_HOME)
        {
            m_currentDir = QDir::home();
        }
        else
        {
            m_currentDir.setPath(path);
        }

        populateLocalFiles();
    }
    else if (msgType == Constant::TYPE_FILE_DOWNLOAD)
    {
        QString cliPath = JsonUtil::getString(object, Constant::KEY_PATH_CLI);
        QString ctlPath = JsonUtil::getString(object, Constant::KEY_PATH_CTL);
        QString transferId = JsonUtil::getString(object, Constant::KEY_TRANSFER_ID);
        if (cliPath.isEmpty() || ctlPath.isEmpty())
        {
            LOG_ERROR("parseFileMsg: Missing file paths for download request");
            return;
        }
        clearTransferCancelled(transferId);
        sendFile(cliPath, ctlPath, transferId);
    }
    else if (msgType == Constant::TYPE_FILE_UPLOAD)
    {
        /* 上传文件现在通过文件通道的二进制数据处理，不再需要输入通道处理 */
        LOG_INFO("File upload request received, waiting for binary data on file channel");
    }
    else if (msgType == Constant::TYPE_RUN_FILE)
    {
        handleRunFile(object);
    }
    else if (msgType == Constant::TYPE_FILE_TRANSFER_CANCEL)
    {
        const QString transferId = JsonUtil::getString(object, Constant::KEY_TRANSFER_ID);
        markTransferCancelled(transferId);
        if (m_filePacketUtil)
        {
            m_filePacketUtil->cancelTransfer(transferId);
            m_filePacketUtil->cancelAllReassemblies();
        }
    }
    else if (msgType == Constant::TYPE_TERMINAL_START ||
             msgType == Constant::TYPE_TERMINAL_INPUT ||
             msgType == Constant::TYPE_TERMINAL_RESIZE ||
             msgType == Constant::TYPE_TERMINAL_STOP)
    {
        handleTerminalMessage(object);
    }
    else
    {
        LOG_WARNING("parseFileMsg: Unknown message type: {}", msgType);
    }
}
