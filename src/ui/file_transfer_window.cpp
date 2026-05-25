#include "file_transfer_window.h"
#include "common/constant.h"
#include "ui/app_title_bar.h"
#include "ui_file_transfer_window.h"
#include "util/json_util.h"
#include "ui/adaptive_ui.h"
#include <QDir>
#include <QDirIterator>
#include <QComboBox>
#include <QPushButton>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QStorageInfo>
#include <QMetaObject>
#include <QMenu>
#include <QAction>
#include <QProcess>
#include <QFileInfo>
#include <QUuid>
#include <QTimer>

FileTransferWindow::FileTransferWindow(QString remoteId, QString remotePwdMd5, WsCli *_ws_cli,
                                       QWidget *parent, WebRtcCtl *sharedRtcCtl)
    : QWidget(parent), ui(new Ui::FileTransferWindow), connected(false), remote_id(remoteId),
      remote_pwd_md5(remotePwdMd5), m_rtc_ctl(remoteId, remotePwdMd5, true),
      m_shared_rtc_ctl(sharedRtcCtl), m_ws(_ws_cli), currentLocalDir(QDir::home())
{
    initUI();
    initCLI();
    if (!m_shared_rtc_ctl)
        emit initRtcCtl();
    else
    {
        QTimer::singleShot(0, this, [this]()
                           { requestRemoteFileList(currentRemotePath.isEmpty() ? Constant::FOLDER_HOME : currentRemotePath); });
        QTimer::singleShot(350, this, [this]()
                           { requestRemoteFileList(currentRemotePath.isEmpty() ? Constant::FOLDER_HOME : currentRemotePath); });
    }
}

FileTransferWindow::~FileTransferWindow()
{
    disconnect();
    if (!m_shared_rtc_ctl && m_rtc_ctl_thread.isRunning())
    {
        QMetaObject::invokeMethod(&m_rtc_ctl, "shutdown", Qt::BlockingQueuedConnection);
    }
    if (!m_shared_rtc_ctl)
        STOP_OBJ_THREAD(m_rtc_ctl_thread);
    delete ui;
}

void FileTransferWindow::initUI()
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setWindowTitle(tr("File transfer: %1").arg(remote_id));
    UiAdaptive::applyAdaptiveWindowSize(this, QSize(900, 600), QSize(520, 360));
    ui->verticalLayout->setContentsMargins(0, 0, 0, 0);
    ui->verticalLayout->insertWidget(0, new AppTitleBar(this, true, true, this));

    dirIcon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
    fileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);

    setupFileTables();
    setupLogTable();

    populateLocalFiles();
    populateRemoteFiles();

    connect(ui->uploadButton, &QPushButton::clicked, this, &FileTransferWindow::onUploadButtonClicked);
    connect(ui->downloadButton, &QPushButton::clicked, this, &FileTransferWindow::onDownloadButtonClicked);

    ui->verticalLayout->setStretch(0, 0);
    ui->verticalLayout->setStretch(1, 1);
    updateLocalPathCombo();
}

void FileTransferWindow::initCLI()
{
    WebRtcCtl *rtcCtl = m_shared_rtc_ctl ? m_shared_rtc_ctl : &m_rtc_ctl;
    if (!m_shared_rtc_ctl)
    {
        connect(&m_rtc_ctl, &WebRtcCtl::sendWsCliBinaryMsg, m_ws, &WsCli::sendWsCliBinaryMsg);
        connect(&m_rtc_ctl, &WebRtcCtl::sendWsCliTextMsg, m_ws, &WsCli::sendWsCliTextMsg);
        connect(m_ws, &WsCli::onWsCliRecvBinaryMsg, &m_rtc_ctl, &WebRtcCtl::onWsCliRecvBinaryMsg);
        connect(m_ws, &WsCli::onWsCliRecvTextMsg, &m_rtc_ctl, &WebRtcCtl::onWsCliRecvTextMsg);
        connect(this, &FileTransferWindow::initRtcCtl, &m_rtc_ctl, &WebRtcCtl::init);

        m_rtc_ctl_thread.setObjectName("FileTransferWindow-WebRtcCtlThread");
        m_rtc_ctl.moveToThread(&m_rtc_ctl_thread);
        m_rtc_ctl_thread.start();
    }

    connect(this, &FileTransferWindow::inputChannelSendMsg, rtcCtl, &WebRtcCtl::inputChannelSendMsg);
    connect(this, &FileTransferWindow::fileChannelSendMsg, rtcCtl, &WebRtcCtl::fileChannelSendMsg);
    connect(this, &FileTransferWindow::fileTextChannelSendMsg, rtcCtl, &WebRtcCtl::fileTextChannelSendMsg);
    connect(this, &FileTransferWindow::uploadFile2CLI, rtcCtl, &WebRtcCtl::uploadFile2CLI);
    connect(this, &FileTransferWindow::cancelFileTransfer, rtcCtl, &WebRtcCtl::cancelFileTransfer, Qt::DirectConnection);

    connect(rtcCtl, &WebRtcCtl::recvGetFileList, this, &FileTransferWindow::recvGetFileList);
    connect(rtcCtl, &WebRtcCtl::recvDownloadFile, this, &FileTransferWindow::recvDownloadFile);
    connect(rtcCtl, &WebRtcCtl::recvUploadFileRes, this, &FileTransferWindow::recvUploadFileRes);
    connect(rtcCtl, &WebRtcCtl::fileTransferProgress, this, &FileTransferWindow::onTransferProgress);
    connect(rtcCtl, &WebRtcCtl::fileTextChannelOpened, this, [this]()
            { requestRemoteFileList(currentRemotePath.isEmpty() ? Constant::FOLDER_HOME : currentRemotePath); });
}
/* Handle Enter key on focused file tables. */
void FileTransferWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        QWidget *focusedWidget = QApplication::focusWidget();
        if (QTableWidget *table = qobject_cast<QTableWidget *>(focusedWidget))
        {
            if (table == ui->localTable)
            {
                QTableWidgetItem *currentItem = table->currentItem();
                if (currentItem)
                {
                    on_localTable_cellDoubleClicked(table->currentRow(), 0);
                }
            }
        }
    }
}
/* Configure file list tables. */
void FileTransferWindow::setupFileTables()
{
    ui->localTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->localTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    /* ui->localTable->setAlternatingRowColors(true); */
    ui->localTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->localTable->setContextMenuPolicy(Qt::CustomContextMenu);
    {
        QVector<int> stretchCols;
        stretchCols.append(0);
        QVector<QPair<int, int>> fixedCols;
        fixedCols.append(qMakePair(1, 90));
        fixedCols.append(qMakePair(2, 150));
        fixedCols.append(qMakePair(3, 90));
        UiAdaptive::makeTableAdaptive(ui->localTable, stretchCols, fixedCols, true);
    }
    ui->localTable->setStyleSheet(QStringLiteral("QTableCornerButton::section { background-color: #202020; border: 1px solid #3a3a3a; }"));
    connect(ui->localTable, &QTableWidget::customContextMenuRequested,
            this, &FileTransferWindow::onLocalTableContextMenuRequested);

    ui->remoteTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->remoteTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    /* ui->remoteTable->setAlternatingRowColors(true); */
    ui->remoteTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->remoteTable->setContextMenuPolicy(Qt::CustomContextMenu);
    {
        QVector<int> stretchCols;
        stretchCols.append(0);
        QVector<QPair<int, int>> fixedCols;
        fixedCols.append(qMakePair(1, 90));
        fixedCols.append(qMakePair(2, 150));
        fixedCols.append(qMakePair(3, 90));
        UiAdaptive::makeTableAdaptive(ui->remoteTable, stretchCols, fixedCols, true);
    }
    ui->remoteTable->setStyleSheet(QStringLiteral("QTableCornerButton::section { background-color: #202020; border: 1px solid #3a3a3a; }"));
    connect(ui->remoteTable, &QTableWidget::customContextMenuRequested,
            this, &FileTransferWindow::onRemoteTableContextMenuRequested);
}
/* Configure transfer log table. */
void FileTransferWindow::setupLogTable()
{
    ui->transferLogTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->transferLogTable->setAlternatingRowColors(false);
    ui->transferLogTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    {
        QVector<int> stretchCols;
        stretchCols.append(0);
        stretchCols.append(1);
        QVector<QPair<int, int>> fixedCols;
        fixedCols.append(qMakePair(2, 210));
        fixedCols.append(qMakePair(3, 90));
        fixedCols.append(qMakePair(4, 90));
        UiAdaptive::makeTableAdaptive(ui->transferLogTable, stretchCols, fixedCols, false);
    }
    ui->transferLogTable->verticalHeader()->setVisible(false);
    ui->transferLogTable->verticalHeader()->setDefaultSectionSize(30);
    ui->transferLogTable->setStyleSheet(QStringLiteral(
        "QTableWidget#transferLogTable {"
        "    background-color: #181818;"
        "    alternate-background-color: #181818;"
        "    color: rgb(131, 193, 224);"
        "    gridline-color: #3a3a3a;"
        "    border: 1px solid #3a3a3a;"
        "    selection-background-color: #783041;"
        "}"
        "QTableWidget#transferLogTable::item {"
        "    background-color: #181818;"
        "    color: rgb(131, 193, 224);"
        "    padding: 4px;"
        "}"
        "QTableWidget#transferLogTable::item:selected {"
        "    background-color: #783041;"
        "    color: #ffffff;"
        "}"
        "QTableWidget#transferLogTable QHeaderView::section {"
        "    background-color: #202020;"
        "    color: rgb(131, 193, 224);"
        "    border: 1px solid #3a3a3a;"
        "    padding: 5px;"
        "}"
        "QTableWidget#transferLogTable QTableCornerButton::section {"
        "    background-color: #202020;"
        "    border: 1px solid #3a3a3a;"
        "}"
        "QTableWidget#transferLogTable QLabel#transferProgressLabel {"
        "    background: transparent;"
        "    color: rgb(131, 193, 224);"
        "    padding: 0 6px;"
        "}"));

    ui->transferLogTable->setColumnCount(5);
    ui->transferLogTable->setHorizontalHeaderLabels({tr("Source path"), tr("Destination path"), tr("Progress"), tr("Status"), tr("Action")});
}
