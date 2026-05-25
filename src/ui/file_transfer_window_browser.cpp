#include "file_transfer_window.h"

#include "common/constant.h"
#include "ui_file_transfer_window.h"
#include "util/config_util.h"
#include "util/json_util.h"

#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStorageInfo>
#include <QTableWidget>

/* Populate local file list. */
void FileTransferWindow::populateLocalFiles()
{
    currentLocalDir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    currentLocalDir.setSorting(QDir::Name | QDir::DirsFirst);
    QFileInfoList list = currentLocalDir.entryInfoList();

    int listSize = list.size() + 1;
    ui->localTable->setRowCount(listSize);

    QTableWidgetItem *item_0_0 = ui->localTable->takeItem(0, 0);
    if (item_0_0)
        delete item_0_0;

    QTableWidgetItem *parentDir = new QTableWidgetItem("..");
    parentDir->setIcon(dirIcon);
    ui->localTable->setItem(0, 0, parentDir);
    for (int i = 0; i < list.size(); i++)
    {
        int row = i + 1;
        const QFileInfo &info = list.at(i);
        QTableWidgetItem *item_0 = ui->localTable->takeItem(row, 0);
        if (item_0)
            delete item_0;
        QTableWidgetItem *item_1 = ui->localTable->takeItem(row, 1);
        if (item_1)
            delete item_1;
        QTableWidgetItem *item_2 = ui->localTable->takeItem(row, 2);
        if (item_2)
            delete item_2;
        QTableWidgetItem *item_3 = ui->localTable->takeItem(row, 3);
        if (item_3)
            delete item_3;

        QDateTime lastModTime = info.lastModified();
        QString lastModTimeStr = lastModTime.toString("yyyy-MM-dd hh:mm:ss");
        QTableWidgetItem *lastModTimeItem = new QTableWidgetItem(lastModTimeStr);
        ui->localTable->setItem(row, 2, lastModTimeItem);

        QString name = info.fileName();
        QTableWidgetItem *nameItem = new QTableWidgetItem(name);
        ui->localTable->setItem(row, 0, nameItem);

        if (info.isDir())
        {
            nameItem->setIcon(dirIcon);
        }
        else
        {
            nameItem->setIcon(fileIcon);
            qint64 size = info.size();
            QString sizeStr = ConvertUtil::formatFileSize(size);
            QTableWidgetItem *sizeItem = new QTableWidgetItem(sizeStr);
            ui->localTable->setItem(row, 1, sizeItem);

            QString fileType = info.suffix();
            QTableWidgetItem *fileTypeItem = new QTableWidgetItem(fileType);
            ui->localTable->setItem(row, 3, fileTypeItem);
        }
    }
}
/* Populate remote file list. */
void FileTransferWindow::populateRemoteFiles()
{
    ui->remoteTable->setRowCount(remoteFiles.size() + 1);
    QTableWidgetItem *item_0_0 = ui->remoteTable->takeItem(0, 0);
    if (item_0_0)
        delete item_0_0;

    QTableWidgetItem *parentDir = new QTableWidgetItem("..");
    parentDir->setIcon(dirIcon);
    ui->remoteTable->setItem(0, 0, parentDir);

    for (int i = 0; i < remoteFiles.size(); i++)
    {
        int row = i + 1;
        QTableWidgetItem *item_0 = ui->remoteTable->takeItem(row, 0);
        if (item_0)
            delete item_0;
        QTableWidgetItem *item_1 = ui->remoteTable->takeItem(row, 1);
        if (item_1)
            delete item_1;
        QTableWidgetItem *item_2 = ui->remoteTable->takeItem(row, 2);
        if (item_2)
            delete item_2;
        QTableWidgetItem *item_3 = ui->remoteTable->takeItem(row, 3);
        if (item_3)
            delete item_3;

        QJsonObject obj = remoteFiles.at(i).toObject();
        QString fileName = JsonUtil::getString(obj, Constant::KEY_NAME);
        if (!fileName.isEmpty())
        {
            QTableWidgetItem *nameItem = new QTableWidgetItem(fileName);
            bool isDir = JsonUtil::getBool(obj, Constant::KEY_IS_DIR);
            nameItem->setData(Qt::UserRole, isDir);
            nameItem->setData(Qt::UserRole + 2, obj.contains(Constant::KEY_FILE_EXECUTABLE));
            nameItem->setData(Qt::UserRole + 1, JsonUtil::getBool(obj, Constant::KEY_FILE_EXECUTABLE, false));
            if (isDir)
            {
                nameItem->setIcon(dirIcon);
            }
            else
            {
                nameItem->setIcon(fileIcon);
            }
            ui->remoteTable->setItem(row, 0, nameItem);
        }

        qint64 fileSize = JsonUtil::getInt64(obj, Constant::KEY_FILE_SIZE);
        if (fileSize > 0)
        {
            QString fileSizeStr = ConvertUtil::formatFileSize(fileSize);
            ui->remoteTable->setItem(row, 1, new QTableWidgetItem(fileSizeStr));
        }

        QString lastModTime = JsonUtil::getString(obj, Constant::KEY_FILE_LAST_MOD_TIME);
        if (!lastModTime.isEmpty())
        {
            ui->remoteTable->setItem(row, 2, new QTableWidgetItem(lastModTime));
        }

        QString fileSuffix = JsonUtil::getString(obj, Constant::KEY_FILE_SUFFIX);
        if (!fileSuffix.isEmpty())
        {
            ui->remoteTable->setItem(row, 3, new QTableWidgetItem(fileSuffix));
        }
    }
}
/* Handle upload button clicks. */
void FileTransferWindow::onUploadButtonClicked()
{
    if (!connected)
    {
        return;
    }

    const QModelIndexList selectedRows = ui->localTable->selectionModel()->selectedRows();
    if (selectedRows.isEmpty())
    {
        return;
    }

    for (const QModelIndex &index : selectedRows)
    {
        QTableWidgetItem *nameItem = ui->localTable->item(index.row(), 0);
        if (!nameItem || nameItem->text() == QStringLiteral(".."))
            continue;

        const QString fileName = nameItem->text();
        const QString localFullPath = QDir::cleanPath(currentLocalDir.absoluteFilePath(fileName));
        if (!QFile::exists(localFullPath))
        {
            LOG_ERROR("File does not exist: {}", localFullPath);
            if (ConfigUtil->showUI)
            {
                QMessageBox::warning(this, tr("Error"), tr("File not found: %1").arg(fileName));
            }
            continue;
        }

        const QString remotePath = ui->remotePathCombo->currentText();
        const QString remoteFullPath = QDir::cleanPath(remotePath + "/" + fileName);

        int fileCount = 0;
        const qint64 totalBytes = collectDirectoryStats(localFullPath, &fileCount);
        TransferTask task;
        task.transferId = createTransferId();
        task.sourcePath = localFullPath;
        task.destinationPath = remoteFullPath;
        task.operation = tr("Upload");
        task.totalBytes = totalBytes;
        task.totalFiles = fileCount;
        m_transferTasks.insert(task.transferId, task);
        updateTransferTaskUi(m_transferTasks[task.transferId]);

        emit uploadFile2CLI(localFullPath, remoteFullPath, task.transferId);
    }
}
void FileTransferWindow::onDownloadButtonClicked()
{
    if (!connected)
    {
        return;
    }

    const QModelIndexList selectedRows = ui->remoteTable->selectionModel()->selectedRows();
    if (selectedRows.isEmpty())
    {
        return;
    }

    for (const QModelIndex &index : selectedRows)
    {
        const int selectedRow = index.row();
        QTableWidgetItem *nameItem = ui->remoteTable->item(selectedRow, 0);
        if (!nameItem || nameItem->text() == QStringLiteral(".."))
            continue;

        const QString fileName = nameItem->text();
        const bool isDirectory = nameItem->data(Qt::UserRole).toBool();
        const QString remoteFullPath = QDir::cleanPath(currentRemotePath + "/" + fileName);
        const QString localFullPath = QDir::cleanPath(currentLocalDir.absolutePath() + "/" + fileName);
        const QJsonObject remoteObject = (selectedRow > 0 && selectedRow - 1 < remoteFiles.size())
                                             ? remoteFiles.at(selectedRow - 1).toObject()
                                             : QJsonObject();
        const qint64 remoteFileSize = JsonUtil::getInt64(remoteObject, Constant::KEY_FILE_SIZE);

        TransferTask task;
        task.transferId = createTransferId();
        task.sourcePath = remoteFullPath;
        task.destinationPath = localFullPath;
        task.operation = tr("Download");
        task.totalBytes = isDirectory ? 0 : remoteFileSize;
        task.totalFiles = isDirectory ? 0 : 1;
        m_transferTasks.insert(task.transferId, task);
        updateTransferTaskUi(m_transferTasks[task.transferId]);

        QJsonObject obj = JsonUtil::createObject()
                              .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_DOWNLOAD)
                              .add(Constant::KEY_PATH_CTL, localFullPath)
                              .add(Constant::KEY_PATH_CLI, remoteFullPath)
                              .add(Constant::KEY_TRANSFER_ID, task.transferId)
                              .add("isDirectory", isDirectory)
                              .build();

        QByteArray msg = JsonUtil::toCompactBytes(obj);
        rtc::message_variant msgStr(msg.toStdString());
        emit fileTextChannelSendMsg(msgStr);
    }
}
/* Handle remote file list response. */
void FileTransferWindow::recvGetFileList(const QJsonObject &object)
{
    LOG_DEBUG("Received file list response for path={}, items={}",
              JsonUtil::getString(object, Constant::KEY_PATH),
              object.value(Constant::KEY_FOLDER_FILES).toArray().size());

    if (!connected)
    {
        connected = true;
    }

    ui->remotePathCombo->clear();
    if (object.contains(Constant::KEY_FOLDER_FILES))
    {
        remoteFiles = object.value(Constant::KEY_FOLDER_FILES).toArray();
        populateRemoteFiles();

        if (object.contains(Constant::KEY_PATH))
        {
            QString receivedPath = JsonUtil::getString(object, Constant::KEY_PATH);
            if (!receivedPath.isEmpty())
            {
                currentRemotePath = receivedPath;
                updateRemotePathCombo();
            }
        }
    }

    if (object.contains(Constant::KEY_FOLDER_MOUNTED))
    {
        QJsonArray mountedList = object.value(Constant::KEY_FOLDER_MOUNTED).toArray();
        for (const QJsonValue &value : mountedList)
        {
            if (value.isString())
            {
                QString mountPath = value.toString();
                if (mountPath != currentRemotePath)
                {
                    ui->remotePathCombo->addItem(mountPath);
                }
            }
        }
        ui->remotePathCombo->setCurrentText(currentRemotePath);
    }
    else
    {
        updateRemotePathCombo();
    }
}
/* Handle local path changes. */
void FileTransferWindow::on_localPathCombo_textActivated(const QString &path)
{
    bool status = true;

    if (QDir::isAbsolutePath(path))
    {
        QDir newDir(path);
        if (newDir.exists())
        {
            currentLocalDir = newDir;
        }
        else
        {
            status = false;
        }
    }
    else
    {
        status = currentLocalDir.cd(path);
    }

    if (status)
    {
        populateLocalFiles();
        updateLocalPathCombo();
    }
}
/* Handle local table double-clicks. */
void FileTransferWindow::on_localTable_cellDoubleClicked(int row, int column)
{
    const QTableWidgetItem *item = ui->localTable->item(row, 0);
    if (!item)
        return;
    QString filePath = item->text();

    bool status = false;
    if (filePath == "..")
    {
        status = currentLocalDir.cdUp();
    }
    else
    {
        status = currentLocalDir.cd(filePath);
    }

    if (status)
    {
        populateLocalFiles();
        updateLocalPathCombo();
    }
}
void FileTransferWindow::on_remotePathCombo_textActivated(const QString &path)
{
    requestRemoteFileList(path);
}
/* Handle remote table double-clicks. */
void FileTransferWindow::on_remoteTable_cellDoubleClicked(int row, int column)
{
    const QTableWidgetItem *item = ui->remoteTable->item(row, 0);
    if (!item)
        return;
    QString path = item->text();
    if (!connected)
    {
        return;
    }

    QString filePath;
    if (path == "..")
    {
        filePath = currentRemotePath.mid(0, currentRemotePath.lastIndexOf('/') + 1);
        if (filePath.isEmpty())
        {
            filePath = Constant::FOLDER_HOME;
        }
    }
    else
    {
        if (!item->data(Qt::UserRole).toBool())
            return;
        filePath = QDir::cleanPath(currentRemotePath + '/' + path);
    }

    requestRemoteFileList(filePath);
}

void FileTransferWindow::requestRemoteFileList(const QString &path)
{
    const QString targetPath = path.isEmpty() ? Constant::FOLDER_HOME : path;
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_LIST)
                          .add(Constant::KEY_PATH, targetPath)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    LOG_DEBUG("Sending file list request for path={}", targetPath);

    rtc::message_variant msgStr(msg.toStdString());
    emit fileTextChannelSendMsg(msgStr);
}

bool FileTransferWindow::isExecutableFileName(const QString &fileName) const
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    return suffix == QStringLiteral("exe") || suffix == QStringLiteral("bat") ||
           suffix == QStringLiteral("cmd") || suffix == QStringLiteral("com") ||
           suffix == QStringLiteral("msi");
#else
    return suffix == QStringLiteral("sh") || suffix == QStringLiteral("run") ||
           suffix == QStringLiteral("appimage") || suffix == QStringLiteral("desktop");
#endif
}

bool FileTransferWindow::isLocalExecutableFile(const QFileInfo &fileInfo) const
{
    if (!fileInfo.isFile())
        return false;

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    return isExecutableFileName(fileInfo.fileName());
#else
    return fileInfo.isExecutable() || isExecutableFileName(fileInfo.fileName());
#endif
}

bool FileTransferWindow::isRemoteExecutableFile(const QTableWidgetItem *item) const
{
    if (!item || item->data(Qt::UserRole).toBool())
        return false;

    const bool hasRemoteExecutableFlag = item->data(Qt::UserRole + 2).toBool();
    if (hasRemoteExecutableFlag)
        return item->data(Qt::UserRole + 1).toBool() || isExecutableFileName(item->text());

    return true;
}

void FileTransferWindow::requestRunFile(bool remoteSide, const QString &filePath)
{
    if (filePath.isEmpty())
        return;

    if (remoteSide)
    {
        QJsonObject obj = JsonUtil::createObject()
                              .add(Constant::KEY_MSGTYPE, Constant::TYPE_RUN_FILE)
                              .add(Constant::KEY_PATH_CLI, filePath)
                              .build();
        rtc::message_variant msgStr(JsonUtil::toCompactBytes(obj).toStdString());
        emit fileTextChannelSendMsg(msgStr);
        LOG_INFO("Requested remote run file: {}", filePath);
    }
    else
    {
        const bool ok = QProcess::startDetached(filePath, QStringList(), QFileInfo(filePath).absolutePath());
        LOG_INFO("Local run file {}: {}", ok ? "succeeded" : "failed", filePath);
    }
}

void FileTransferWindow::onLocalTableContextMenuRequested(const QPoint &pos)
{
    if (!ConfigUtil->showUI)
        return;

    const int row = ui->localTable->rowAt(pos.y());
    QTableWidgetItem *item = row >= 0 ? ui->localTable->item(row, 0) : nullptr;
    if (!item || item->text() == QStringLiteral(".."))
        return;

    const QString fileName = item->text();
    const QString filePath = QDir::cleanPath(currentLocalDir.absolutePath() + "/" + fileName);
    QFileInfo info(filePath);
    if (!isLocalExecutableFile(info))
        return;

    QMenu menu(this);
    QAction *runAction = menu.addAction(tr("Run"));
    if (menu.exec(ui->localTable->viewport()->mapToGlobal(pos)) == runAction)
        requestRunFile(false, filePath);
}

void FileTransferWindow::onRemoteTableContextMenuRequested(const QPoint &pos)
{
    if (!ConfigUtil->showUI)
        return;

    const int row = ui->remoteTable->rowAt(pos.y());
    QTableWidgetItem *item = row >= 0 ? ui->remoteTable->item(row, 0) : nullptr;
    if (!item || item->text() == QStringLiteral("..") || !connected)
        return;

    const bool isDir = item->data(Qt::UserRole).toBool();
    const QString fileName = item->text();
    if (isDir || !isRemoteExecutableFile(item))
        return;

    const QString filePath = QDir::cleanPath(currentRemotePath + "/" + fileName);
    QMenu menu(this);
    QAction *runAction = menu.addAction(tr("Run"));
    if (menu.exec(ui->remoteTable->viewport()->mapToGlobal(pos)) == runAction)
        requestRunFile(true, filePath);
}

void FileTransferWindow::recvDownloadFile(bool status, const QString &filePath)
{
    finishTransferTask(QString(), status, filePath);
    if (status)
        populateLocalFiles();
}
/* Update local path selector. */
void FileTransferWindow::updateLocalPathCombo()
{
    ui->localPathCombo->clear();

    QString currentPath = currentLocalDir.absolutePath();
    ui->localPathCombo->addItem(currentPath);
    QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &volume : volumes)
    {
        if (volume.isValid() && volume.isReady())
        {
            ui->localPathCombo->addItem(volume.rootPath());
        }
    }

    ui->localPathCombo->setCurrentText(currentPath);
}
/* Update remote path selector. */
void FileTransferWindow::updateRemotePathCombo()
{
    ui->remotePathCombo->clear();

    if (!currentRemotePath.isEmpty())
    {
        ui->remotePathCombo->addItem(currentRemotePath);
        ui->remotePathCombo->setCurrentText(currentRemotePath);
    }
}

void FileTransferWindow::recvUploadFileRes(bool status, const QString &filePath)
{
    if (status)
    {
        on_remotePathCombo_textActivated(ui->remotePathCombo->currentText());
    }
    finishTransferTask(QString(), status, filePath);
}

void FileTransferWindow::onTransferProgress(const QString &transferId, qint64 transferredBytes, qint64 totalBytes,
                                            int transferredFiles, int totalFiles)
{
    TransferTask *task = findTransferTaskById(transferId);
    if (!task || task->canceled || task->completed)
        return;

    task->transferredBytes = qMax<qint64>(0, transferredBytes);
    if (totalBytes >= 0)
        task->totalBytes = totalBytes;
    task->transferredFiles = qMax(0, transferredFiles);
    if (totalFiles >= 0)
        task->totalFiles = totalFiles;
    if (task->row >= 0)
        ui->transferLogTable->setItem(task->row, 3, new QTableWidgetItem(tr("Transferring")));
    updateProgressCell(*task);
}

void FileTransferWindow::onTransferCancelClicked()
{
    auto *button = qobject_cast<QPushButton *>(sender());
    if (!button)
        return;
    cancelTransferTask(button->property("transferId").toString());
}
