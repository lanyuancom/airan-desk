#include "file_transfer_window.h"

#include "ui_file_transfer_window.h"
#include "util/convert_util.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QUuid>
#include <QWidget>

QString FileTransferWindow::createTransferId() const
{
    QString uuid = QUuid::createUuid().toString();
    if (uuid.startsWith(QLatin1Char('{')) && uuid.endsWith(QLatin1Char('}')))
        return uuid.mid(1, uuid.size() - 2);
    return uuid;
}

qint64 FileTransferWindow::collectDirectoryStats(const QString &path, int *fileCount) const
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

FileTransferWindow::TransferTask *FileTransferWindow::findTransferTaskById(const QString &transferId)
{
    if (transferId.isEmpty() || !m_transferTasks.contains(transferId))
        return nullptr;
    return &m_transferTasks[transferId];
}

FileTransferWindow::TransferTask *FileTransferWindow::findTransferTaskByPath(const QString &path)
{
    TransferTask *completedMatch = nullptr;
    int completedRow = -1;
    TransferTask *pendingMatch = nullptr;
    int pendingRow = -1;

    for (auto it = m_transferTasks.begin(); it != m_transferTasks.end(); ++it)
    {
        if (it->sourcePath == path || it->destinationPath == path)
        {
            TransferTask *task = &it.value();
            if (!task->completed && !task->canceled)
            {
                if (task->row >= pendingRow)
                {
                    pendingRow = task->row;
                    pendingMatch = task;
                }
            }
            else if (task->row >= completedRow)
            {
                completedRow = task->row;
                completedMatch = task;
            }
        }
    }

    return pendingMatch ? pendingMatch : completedMatch;
}

int FileTransferWindow::ensureTransferTaskRow(const TransferTask &task)
{
    if (task.row >= 0)
        return task.row;

    const int row = ui->transferLogTable->rowCount();
    ui->transferLogTable->insertRow(row);
    return row;
}

void FileTransferWindow::updateProgressCell(TransferTask &task)
{
    if (!task.progressLabel)
        return;

    const qint64 totalBytes = qMax<qint64>(0, task.totalBytes);
    const qint64 transferredBytes = qBound<qint64>(0, task.transferredBytes, totalBytes > 0 ? totalBytes : task.transferredBytes);
    const int totalFiles = qMax(0, task.totalFiles);
    const int transferredFiles = qBound(0, task.transferredFiles, totalFiles > 0 ? totalFiles : task.transferredFiles);

    QString progressText;
    if (totalFiles > 1)
    {
        progressText = tr("%1/%2 files, %3 / %4")
                           .arg(transferredFiles)
                           .arg(totalFiles)
                           .arg(ConvertUtil::formatFileSize(transferredBytes))
                           .arg(ConvertUtil::formatFileSize(totalBytes));
    }
    else
    {
        progressText = tr("%1 / %2")
                           .arg(ConvertUtil::formatFileSize(transferredBytes))
                           .arg(totalBytes > 0 ? ConvertUtil::formatFileSize(totalBytes) : tr("Unknown"));
    }
    task.progressLabel->setText(progressText);
}

void FileTransferWindow::updateTransferTaskUi(TransferTask &task)
{
    if (task.row < 0)
        task.row = ensureTransferTaskRow(task);

    ui->transferLogTable->setItem(task.row, 0, new QTableWidgetItem(task.sourcePath));
    ui->transferLogTable->setItem(task.row, 1, new QTableWidgetItem(task.destinationPath));
    ui->transferLogTable->setItem(task.row, 3, new QTableWidgetItem(tr("Waiting")));

    auto *progressLabel = new QLabel(ui->transferLogTable);
    progressLabel->setObjectName(QStringLiteral("transferProgressLabel"));
    progressLabel->setAlignment(Qt::AlignCenter);
    progressLabel->setStyleSheet(QStringLiteral("background: transparent; color: rgb(131, 193, 224); padding: 0 6px;"));
    ui->transferLogTable->setCellWidget(task.row, 2, progressLabel);
    task.progressLabel = progressLabel;

    auto *cancelButton = new QPushButton(tr("Cancel"), ui->transferLogTable);
    cancelButton->setObjectName(QStringLiteral("transferActionButton"));
    cancelButton->setText(QStringLiteral("X"));
    cancelButton->setToolTip(tr("Cancel"));
    cancelButton->setCursor(Qt::PointingHandCursor);
    cancelButton->setFlat(true);
    cancelButton->setMinimumSize(32, 24);
    cancelButton->setMaximumHeight(24);
    cancelButton->setStyleSheet(QStringLiteral(
        "QPushButton#transferActionButton {"
        "    background: transparent;"
        "    border: none;"
        "    color: #ff7a90;"
        "    font-size: 15px;"
        "    font-weight: 700;"
        "    padding: 0;"
        "}"
        "QPushButton#transferActionButton:hover {"
        "    color: #ffffff;"
        "    background: transparent;"
        "}"
        "QPushButton#transferActionButton:disabled {"
        "    color: #7a7d82;"
        "    background: transparent;"
        "}"));
    cancelButton->setProperty("transferId", task.transferId);
    connect(cancelButton, &QPushButton::clicked, this, &FileTransferWindow::onTransferCancelClicked);
    auto *actionHost = new QWidget(ui->transferLogTable);
    actionHost->setStyleSheet(QStringLiteral("background: #181818;"));
    auto *actionLayout = new QHBoxLayout(actionHost);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(0);
    actionLayout->addStretch(1);
    actionLayout->addWidget(cancelButton);
    actionLayout->addStretch(1);
    ui->transferLogTable->setCellWidget(task.row, 4, actionHost);
    task.cancelButton = cancelButton;

    updateProgressCell(task);
}

void FileTransferWindow::finishTransferTask(const QString &transferId, bool status, const QString &filePath)
{
    TransferTask *task = findTransferTaskById(transferId);
    if (!task)
        task = findTransferTaskByPath(filePath);
    if (!task || task->row < 0 || task->canceled)
        return;
    task->completed = true;

    if (status && task->totalBytes > 0)
        task->transferredBytes = task->totalBytes;
    if (status && task->totalFiles > 0)
        task->transferredFiles = task->totalFiles;
    updateProgressCell(*task);

    ui->transferLogTable->setItem(task->row, 3, new QTableWidgetItem(status ? tr("Succeeded") : tr("Failed")));
    if (task->cancelButton)
    {
        task->cancelButton->setEnabled(false);
        task->cancelButton->setText(status ? QStringLiteral("✓") : QStringLiteral("X"));
        task->cancelButton->setToolTip(status ? tr("Succeeded") : tr("Failed"));
        if (status)
            task->cancelButton->setStyleSheet(task->cancelButton->styleSheet() + QStringLiteral("QPushButton#transferActionButton:disabled { color: #77d68a; }"));
        task->cancelButton->setCursor(Qt::ArrowCursor);
    }
}

void FileTransferWindow::cancelTransferTask(const QString &transferId)
{
    TransferTask *task = findTransferTaskById(transferId);
    if (!task)
        return;

    task->canceled = true;
    task->completed = true;
    ui->transferLogTable->setItem(task->row, 3, new QTableWidgetItem(tr("Canceled")));
    if (task->cancelButton)
    {
        task->cancelButton->setEnabled(false);
        task->cancelButton->setToolTip(tr("Canceled"));
        task->cancelButton->setText(QStringLiteral("X"));
        task->cancelButton->setCursor(Qt::ArrowCursor);
    }
    emit cancelFileTransfer(transferId);
}
