#include "terminal/terminal_file_panel.h"
#include "common/constant.h"
#include "util/convert_util.h"
#include "util/config_util.h"
#include "util/json_util.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QPixmap>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QTransform>
#include <QVBoxLayout>
#include "ui/adaptive_ui.h"

namespace
{
    QIcon rotatedIcon(const QIcon &sourceIcon, const QSize &size, qreal degrees)
    {
        const QPixmap sourcePixmap = sourceIcon.pixmap(size);
        if (sourcePixmap.isNull())
            return sourceIcon;

        return QIcon(sourcePixmap.transformed(QTransform().rotate(degrees), Qt::SmoothTransformation));
    }
} /* namespace */

TerminalFilePanel::TerminalFilePanel(QWidget *parent)
    : QWidget(parent)
{
    m_dirIcon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
    m_fileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    setupUi();
}

QString TerminalFilePanel::currentRemotePath() const
{
    return m_currentRemotePath;
}

void TerminalFilePanel::setupUi()
{
    setMinimumWidth(200);
    setObjectName(QStringLiteral("terminalFilePanel"));
    setStyleSheet(QStringLiteral(
        "#terminalFilePanel { background: #181818; color: rgb(131, 193, 224); }"
        "#terminalFilePanel QLineEdit,"
        "#terminalFilePanel QComboBox {"
        "    background: #181818;"
        "    color: rgb(131, 193, 224);"
        "    border: 1px solid #858585;"
        "    border-radius: 4px;"
        "    padding: 2px 6px;"
        "    min-height: 18px;"
        "}"
        "#terminalFilePanel QComboBox::drop-down { border: none; width: 16px; }"
        "#terminalFilePanel QComboBox QAbstractItemView {"
        "    background: #242424;"
        "    color: #e5eaf3;"
        "    border: 1px solid #4c4d4f;"
        "    selection-background-color: #783041;"
        "}"
        "#terminalFilePanel QCheckBox {"
        "    background: transparent;"
        "    color: rgb(131, 193, 224);"
        "    spacing: 4px;"
        "}"
        "#terminalFilePanel QCheckBox::indicator { width: 12px; height: 12px; }"
        "#terminalFilePanel QTableWidget {"
        "    background: #181818;"
        "    alternate-background-color: #181818;"
        "    color: rgb(131, 193, 224);"
        "    border: 1px solid #3a3a3a;"
        "    gridline-color: transparent;"
        "    selection-background-color: #783041;"
        "}"
        "#terminalFilePanel QTableWidget::item {"
        "    background: #181818;"
        "    color: rgb(131, 193, 224);"
        "    padding: 3px;"
        "}"
        "#terminalFilePanel QTableWidget::item:selected {"
        "    background: #783041;"
        "    color: #ffffff;"
        "}"
        "#terminalFilePanel QHeaderView::section {"
        "    background: #202020;"
        "    color: rgb(131, 193, 224);"
        "    border: 1px solid #3a3a3a;"
        "    padding: 4px;"
        "}"
        "#terminalFilePanel QToolButton {"
        "    background: transparent;"
        "    border: 1px solid transparent;"
        "    border-radius: 3px;"
        "    color: rgb(131, 193, 224);"
        "    padding: 1px;"
        "}"
        "#terminalFilePanel QToolButton:hover {"
        "    background: #242424;"
        "    border-color: #4c4d4f;"
        "}"
        "#terminalFilePanel QToolButton:disabled { color: #777777; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(3, 3, 3, 3);
    layout->setSpacing(2);

    auto *toolbar = new QHBoxLayout();
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(2);

    m_parentButton = new QToolButton(this);
    m_parentButton->setText(QStringLiteral("↑"));
    m_parentButton->setToolTip(tr("上级目录"));

    m_refreshButton = new QToolButton(this);
    m_refreshButton->setIcon(QApplication::style()->standardIcon(QStyle::SP_BrowserReload));
    m_refreshButton->setToolTip(tr("刷新"));

    m_downloadButton = new QToolButton(this);
    m_downloadButton->setText(QStringLiteral("↓"));
    m_downloadButton->setToolTip(tr("下载"));

    m_uploadFileButton = new QToolButton(this);
    m_uploadFileButton->setText(QStringLiteral("↑F"));
    m_uploadFileButton->setToolTip(tr("上传文件"));

    m_uploadDirectoryButton = new QToolButton(this);
    m_uploadDirectoryButton->setText(QStringLiteral("↑D"));
    m_uploadDirectoryButton->setToolTip(tr("上传目录"));

    m_parentButton->setText(QString());
    m_parentButton->setIcon(QApplication::style()->standardIcon(QStyle::SP_ArrowLeft));
    const QIcon downloadIcon = QApplication::style()->standardIcon(QStyle::SP_ArrowDown);
    const QIcon uploadIcon = rotatedIcon(downloadIcon, QSize(14, 14), 180);
    m_downloadButton->setText(QString());
    m_downloadButton->setIcon(downloadIcon);
    m_uploadFileButton->setText(QString());
    m_uploadFileButton->setIcon(uploadIcon);
    m_uploadDirectoryButton->setText(QString());
    m_uploadDirectoryButton->setIcon(QApplication::style()->standardIcon(QStyle::SP_DirIcon));
    const QList<QToolButton *> toolButtons = {
        m_parentButton,
        m_refreshButton,
        m_downloadButton,
        m_uploadFileButton,
        m_uploadDirectoryButton,
    };
    for (QToolButton *button : toolButtons)
    {
        button->setFixedSize(24, 22);
        button->setIconSize(QSize(14, 14));
        button->setAutoRaise(true);
    }

    m_driveCombo = new QComboBox(this);
    m_driveCombo->setToolTip(tr("驱动器"));
    m_driveCombo->setFixedSize(58, 22);
    m_driveCombo->setMinimumContentsLength(3);
    m_driveCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_driveCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_followPathCheck = new QCheckBox(tr("跟随终端"), this);
    m_followPathCheck->setChecked(true);
    m_followPathCheck->setToolTip(tr("跟随终端路径"));

    m_followPathCheck->setFixedHeight(22);

    toolbar->addWidget(m_parentButton);
    toolbar->addWidget(m_refreshButton);
    toolbar->addWidget(m_downloadButton);
    toolbar->addWidget(m_uploadFileButton);
    toolbar->addWidget(m_uploadDirectoryButton);
    toolbar->addWidget(m_driveCombo);
    toolbar->addWidget(m_followPathCheck);
    toolbar->addStretch();

    m_remotePathEdit = new QLineEdit(this);
    m_remotePathEdit->setPlaceholderText(tr("远程路径"));

    m_remotePathEdit->setFixedHeight(24);

    m_remoteTable = new QTableWidget(this);
    m_remoteTable->setColumnCount(3);
    m_remoteTable->setHorizontalHeaderLabels({tr("名称"), tr("大小"), tr("修改时间")});
    m_remoteTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_remoteTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_remoteTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_remoteTable->setShowGrid(false);
    m_remoteTable->verticalHeader()->setVisible(false);
    m_remoteTable->horizontalHeader()->setSectionsMovable(false);
    {
        QVector<int> stretchCols;
        stretchCols.append(0);
        QVector<QPair<int, int>> fixedCols;
        fixedCols.append(qMakePair(1, 72));
        fixedCols.append(qMakePair(2, 132));
        UiAdaptive::makeTableAdaptive(m_remoteTable, stretchCols, fixedCols, false);
    }
    m_remoteTable->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_remoteTable->verticalHeader()->setDefaultSectionSize(27);
    m_remoteTable->setAlternatingRowColors(false);

    layout->addLayout(toolbar);
    layout->addWidget(m_remotePathEdit);
    layout->addWidget(m_remoteTable, 1);

    connect(m_remotePathEdit, &QLineEdit::returnPressed, this, &TerminalFilePanel::onPathEditingFinished);
    connect(m_remotePathEdit, &QLineEdit::editingFinished, this, &TerminalFilePanel::onPathEditingFinished);
    connect(m_parentButton, &QToolButton::clicked, this, &TerminalFilePanel::onParentClicked);
    connect(m_refreshButton, &QToolButton::clicked, this, &TerminalFilePanel::onRefreshClicked);
    connect(m_downloadButton, &QToolButton::clicked, this, &TerminalFilePanel::onDownloadClicked);
    connect(m_uploadFileButton, &QToolButton::clicked, this, &TerminalFilePanel::onUploadFileClicked);
    connect(m_uploadDirectoryButton, &QToolButton::clicked, this, &TerminalFilePanel::onUploadDirectoryClicked);
    connect(m_driveCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TerminalFilePanel::onDriveChanged);
    connect(m_remoteTable, &QTableWidget::cellDoubleClicked, this, &TerminalFilePanel::onRemoteCellDoubleClicked);

    setConnected(false);
    updatePathEdit();
}

void TerminalFilePanel::setConnected(bool connected)
{
    m_connected = connected;
    m_remotePathEdit->setEnabled(connected);
    m_parentButton->setEnabled(connected);
    m_refreshButton->setEnabled(connected);
    m_uploadFileButton->setEnabled(connected);
    m_uploadDirectoryButton->setEnabled(connected);
    m_downloadButton->setEnabled(connected);
    m_driveCombo->setEnabled(connected);
    m_followPathCheck->setEnabled(connected);
}

void TerminalFilePanel::setRemotePath(const QString &path)
{
    if (path.isEmpty() || path == m_currentRemotePath)
        return;

    m_currentRemotePath = path;
    updatePathEdit();
    if (m_connected)
        emit requestFileList(path);
}

void TerminalFilePanel::followTerminalPath(const QString &path)
{
    if (m_followPathCheck && !m_followPathCheck->isChecked())
        return;
    setRemotePath(path);
}

void TerminalFilePanel::recvGetFileList(const QJsonObject &object)
{
    if (object.contains(Constant::KEY_PATH))
    {
        const QString path = JsonUtil::getString(object, Constant::KEY_PATH);
        if (!path.isEmpty())
            m_currentRemotePath = path;
    }
    if (object.contains(Constant::KEY_FOLDER_FILES))
    {
        m_remoteFiles = object.value(Constant::KEY_FOLDER_FILES).toArray();
        populateRemoteFiles();
    }
    if (object.contains(Constant::KEY_FOLDER_MOUNTED))
        updateMountedPaths(object.value(Constant::KEY_FOLDER_MOUNTED).toArray());
    setConnected(true);
    updatePathEdit();
    updateDriveCombo();
}

void TerminalFilePanel::recvDownloadFile(bool, const QString &)
{
}

void TerminalFilePanel::recvUploadFile(bool status, const QString &)
{
    if (status && !m_currentRemotePath.isEmpty())
        emit requestFileList(m_currentRemotePath);
}

void TerminalFilePanel::populateRemoteFiles()
{
    m_remoteTable->setRowCount(m_remoteFiles.size() + 1);
    auto *parent = new QTableWidgetItem(QStringLiteral(".."));
    parent->setIcon(m_dirIcon);
    parent->setData(Qt::UserRole, true);
    m_remoteTable->setItem(0, 0, parent);
    m_remoteTable->setItem(0, 1, new QTableWidgetItem());
    m_remoteTable->setItem(0, 2, new QTableWidgetItem());

    for (int i = 0; i < m_remoteFiles.size(); ++i)
    {
        const QJsonObject obj = m_remoteFiles.at(i).toObject();
        const int row = i + 1;
        const bool isDir = JsonUtil::getBool(obj, Constant::KEY_IS_DIR);
        auto *nameItem = new QTableWidgetItem(JsonUtil::getString(obj, Constant::KEY_NAME));
        nameItem->setIcon(isDir ? m_dirIcon : m_fileIcon);
        nameItem->setData(Qt::UserRole, isDir);
        m_remoteTable->setItem(row, 0, nameItem);

        const qint64 size = JsonUtil::getInt64(obj, Constant::KEY_FILE_SIZE);
        m_remoteTable->setItem(row, 1, new QTableWidgetItem(isDir || size <= 0 ? QString() : ConvertUtil::formatFileSize(size)));
        m_remoteTable->setItem(row, 2, new QTableWidgetItem(JsonUtil::getString(obj, Constant::KEY_FILE_LAST_MOD_TIME)));
    }
}

void TerminalFilePanel::updatePathEdit()
{
    if (m_remotePathEdit->text() != m_currentRemotePath)
        m_remotePathEdit->setText(m_currentRemotePath);
}

void TerminalFilePanel::updateMountedPaths(const QJsonArray &paths)
{
    QStringList mountedPaths;
    for (const QJsonValue &value : paths)
    {
        const QString path = value.toString();
        if (!path.isEmpty() && !mountedPaths.contains(path))
            mountedPaths.append(path);
    }

    if (mountedPaths != m_mountedPaths)
    {
        m_mountedPaths = mountedPaths;
        updateDriveCombo();
    }
}

void TerminalFilePanel::updateDriveCombo()
{
    if (!m_driveCombo)
        return;

    m_updatingDriveCombo = true;
    m_driveCombo->clear();
    for (const QString &path : m_mountedPaths)
        m_driveCombo->addItem(QDir::toNativeSeparators(path), path);

    const QString current = QDir::fromNativeSeparators(m_currentRemotePath);
    int selectedIndex = -1;
    for (int i = 0; i < m_mountedPaths.size(); ++i)
    {
        const QString mount = QDir::fromNativeSeparators(m_mountedPaths.at(i));
        if (current.startsWith(mount, Qt::CaseInsensitive))
        {
            selectedIndex = i;
            break;
        }
    }

    if (selectedIndex >= 0)
        m_driveCombo->setCurrentIndex(selectedIndex);
    m_updatingDriveCombo = false;
}

QString TerminalFilePanel::joinRemotePath(const QString &basePath, const QString &name) const
{
    if (basePath.endsWith('/') || basePath.endsWith('\\'))
        return basePath + name;

    const QChar separator = basePath.contains('\\') && !basePath.contains('/') ? QLatin1Char('\\') : QLatin1Char('/');
    return basePath + separator + name;
}

QString TerminalFilePanel::parentRemotePath(const QString &path) const
{
    const QString normalized = QDir::cleanPath(path);
    const bool windowsDriveRoot = normalized.size() == 3 &&
                                  normalized.at(1) == QLatin1Char(':') &&
                                  (normalized.at(2) == QLatin1Char('/') || normalized.at(2) == QLatin1Char('\\'));
    if (windowsDriveRoot)
        return normalized;

    const bool windowsDrivePath = normalized.size() >= 3 &&
                                  normalized.at(1) == QLatin1Char(':') &&
                                  (normalized.at(2) == QLatin1Char('/') || normalized.at(2) == QLatin1Char('\\'));
    const int slash = normalized.lastIndexOf('/');
    const int backslash = normalized.lastIndexOf('\\');
    const int index = qMax(slash, backslash);
    if (windowsDrivePath && index <= 2)
        return normalized.left(3);
    if (index <= 0)
        return slash == 0 ? QStringLiteral("/") : normalized;
    return normalized.left(index);
}

void TerminalFilePanel::onPathEditingFinished()
{
    const QString path = m_remotePathEdit->text().trimmed();
    if (m_connected && !path.isEmpty() && path != m_currentRemotePath)
        setRemotePath(path);
}

void TerminalFilePanel::onParentClicked()
{
    if (m_connected && !m_currentRemotePath.isEmpty())
        setRemotePath(parentRemotePath(m_currentRemotePath));
}

void TerminalFilePanel::onRefreshClicked()
{
    if (m_connected && !m_currentRemotePath.isEmpty())
        emit requestFileList(m_currentRemotePath);
}

void TerminalFilePanel::onDownloadClicked()
{
    if (!ConfigUtil->showUI)
        return;

    const QModelIndexList selectedRows = m_remoteTable->selectionModel()->selectedRows();
    if (selectedRows.isEmpty() || m_currentRemotePath.isEmpty())
        return;

    const QString localDir = QFileDialog::getExistingDirectory(this, tr("选择下载目录"), QDir::homePath());
    if (localDir.isEmpty())
        return;

    for (const QModelIndex &index : selectedRows)
    {
        const int row = index.row();
        QTableWidgetItem *item = row >= 0 ? m_remoteTable->item(row, 0) : nullptr;
        if (!item || item->text() == QStringLiteral(".."))
            continue;

        const QString fileName = item->text();
        const bool isDir = item->data(Qt::UserRole).toBool();
        const QString remotePath = joinRemotePath(m_currentRemotePath, fileName);
        const QString localPath = QDir::cleanPath(localDir + "/" + fileName);
        emit requestDownload(remotePath, localPath, isDir);
    }
}

void TerminalFilePanel::onUploadFileClicked()
{
    if (!ConfigUtil->showUI)
        return;

    if (!m_connected)
        return;

    const QString localPath = QFileDialog::getOpenFileName(this, tr("选择上传文件"), QDir::homePath());
    if (localPath.isEmpty())
        return;

    const QFileInfo info(localPath);
    if (!info.exists() || !info.isFile())
    {
        if (ConfigUtil->showUI)
            QMessageBox::warning(this, tr("错误"), tr("文件不存在: %1").arg(localPath));
        return;
    }

    const QString remoteBasePath = m_currentRemotePath.isEmpty() && m_remotePathEdit
                                       ? m_remotePathEdit->text().trimmed()
                                       : m_currentRemotePath;
    if (remoteBasePath.isEmpty())
    {
        QMessageBox::warning(this, tr("错误"), tr("请先选择远程目录"));
        return;
    }

    emit requestUpload(localPath, joinRemotePath(remoteBasePath, info.fileName()), false);
}

void TerminalFilePanel::onUploadDirectoryClicked()
{
    if (!ConfigUtil->showUI)
        return;

    if (!m_connected)
        return;

    const QString localPath = QFileDialog::getExistingDirectory(this, tr("选择上传目录"), QDir::homePath());
    if (localPath.isEmpty())
        return;

    const QFileInfo info(localPath);
    if (!info.exists() || !info.isDir())
    {
        if (ConfigUtil->showUI)
            QMessageBox::warning(this, tr("错误"), tr("文件不存在: %1").arg(localPath));
        return;
    }

    const QString remoteBasePath = m_currentRemotePath.isEmpty() && m_remotePathEdit
                                       ? m_remotePathEdit->text().trimmed()
                                       : m_currentRemotePath;
    if (remoteBasePath.isEmpty())
    {
        QMessageBox::warning(this, tr("错误"), tr("请先选择远程目录"));
        return;
    }

    emit requestUpload(localPath, joinRemotePath(remoteBasePath, info.fileName()), true);
}

void TerminalFilePanel::onDriveChanged(int index)
{
    if (m_updatingDriveCombo || !m_connected || index < 0 || !m_driveCombo)
        return;

    const QString path = m_driveCombo->itemData(index).toString();
    if (!path.isEmpty())
        setRemotePath(path);
}

void TerminalFilePanel::onRemoteCellDoubleClicked(int row, int)
{
    QTableWidgetItem *item = row >= 0 ? m_remoteTable->item(row, 0) : nullptr;
    if (!item || !m_connected || m_currentRemotePath.isEmpty())
        return;

    const bool isDir = item->data(Qt::UserRole).toBool();
    if (!isDir)
        return;

    const QString nextPath = item->text() == QStringLiteral("..")
                                 ? parentRemotePath(m_currentRemotePath)
                                 : joinRemotePath(m_currentRemotePath, item->text());
    setRemotePath(nextPath);
}
