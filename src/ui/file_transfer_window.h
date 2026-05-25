#ifndef FILETRANSFERTOOL_H
#define FILETRANSFERTOOL_H

#include <QWidget>
#include <QLabel>
#include <QStyle>
#include <QDir>
#include <QIcon>
#include <QTableWidgetItem>
#include <QPoint>
#include <QHash>
#include "common/constant.h"
#include "webrtc/webrtc_ctl.h"
#include "websocket/ws_cli.h"

class QFileInfo;
class QComboBox;
class QGroupBox;
class QKeyEvent;
class QPushButton;
class QTableWidget;

namespace Ui
{
    class FileTransferWindow;
}

class FileTransferWindow : public QWidget
{
    Q_OBJECT
public:
    /* 构造函数：传入远程ID、远程密码MD5、WebSocket客户端实例 */
    explicit FileTransferWindow(QString remoteId, QString remotePwdMd5, WsCli *_ws_cli,
                                QWidget *parent = nullptr, WebRtcCtl *sharedRtcCtl = nullptr);
    ~FileTransferWindow();
    /* 初始化UI界面 */
    void initUI();
    /* 初始化WebRTC控制器 */
    void initCLI();
    /* 处理键盘事件 */
    void keyPressEvent(QKeyEvent *event) override;
signals:
    /* 信号：发送消息到输入数据通道 */
    void inputChannelSendMsg(const rtc::message_variant &data);
    /* 信号：发送消息到文件数据通道 */
    void fileChannelSendMsg(const rtc::message_variant &data);
    /* 信号：发送消息到文件文本数据通道 */
    void fileTextChannelSendMsg(const rtc::message_variant &data);
    /* 信号：初始化webrtc */
    void initRtcCtl();
    /* 信号：上传文件到CLI */
    void uploadFile2CLI(const QString &ctlPath, const QString &cliPath, const QString &transferId);
    void cancelFileTransfer(const QString &transferId);
public slots:
    /* 处理上传按钮点击事件 */
    void onUploadButtonClicked();
    /* 处理下载按钮点击事件 */
    void onDownloadButtonClicked();
    /* 接收到被控端文件列表 */
    void recvGetFileList(const QJsonObject &object);
    /* 接收到被控端文件 */
    void recvDownloadFile(bool status, const QString &filePath);
    void recvUploadFileRes(bool status, const QString &filePath);
    void onTransferProgress(const QString &transferId, qint64 transferredBytes, qint64 totalBytes, int transferredFiles, int totalFiles);
private slots:
    /* 处理本地路径选择变化 */
    void on_localPathCombo_textActivated(const QString &path);
    /* 处理本地文件列表双击事件 */
    void on_localTable_cellDoubleClicked(int row, int column);

    void on_remotePathCombo_textActivated(const QString &path);
    /* 处理远程路径选择变化 */
    void on_remoteTable_cellDoubleClicked(int row, int column);
    void onLocalTableContextMenuRequested(const QPoint &pos);
    void onRemoteTableContextMenuRequested(const QPoint &pos);
    void onTransferCancelClicked();

private:
    struct TransferTask
    {
        QString transferId;
        QString sourcePath;
        QString destinationPath;
        QString operation;
        qint64 transferredBytes{0};
        qint64 totalBytes{0};
        int transferredFiles{0};
        int totalFiles{0};
        int row{-1};
        bool canceled{false};
        bool completed{false};
        QLabel *progressLabel{nullptr};
        QPushButton *cancelButton{nullptr};
    };

    /* 设置本地文件列表 */
    void populateLocalFiles();
    /* 设置远程文件列表 */
    void populateRemoteFiles();
    /* 设置文件列表 */
    void setupFileTables();
    /* 设置传输记录表格 */
    void setupLogTable();
    bool isExecutableFileName(const QString &fileName) const;
    bool isLocalExecutableFile(const QFileInfo &fileInfo) const;
    bool isRemoteExecutableFile(const QTableWidgetItem *item) const;
    void requestRunFile(bool remoteSide, const QString &filePath);
    /* 更新本地路径显示 */
    void updateLocalPathCombo();
    /* 更新远程路径显示 */
    void updateRemotePathCombo();
    void requestRemoteFileList(const QString &path);
    QString createTransferId() const;
    qint64 collectDirectoryStats(const QString &path, int *fileCount) const;
    TransferTask *findTransferTaskById(const QString &transferId);
    TransferTask *findTransferTaskByPath(const QString &path);
    int ensureTransferTaskRow(const TransferTask &task);
    void updateTransferTaskUi(TransferTask &task);
    void finishTransferTask(const QString &transferId, bool status, const QString &filePath);
    void cancelTransferTask(const QString &transferId);
    void updateProgressCell(TransferTask &task);

private:
    Ui::FileTransferWindow *ui;
    bool connected;
    QLabel label;
    QString remote_id;
    QString remote_pwd_md5;
    WebRtcCtl m_rtc_ctl;
    WebRtcCtl *m_shared_rtc_ctl{nullptr};
    WsCli *m_ws;
    QThread m_rtc_ctl_thread;

    QDir currentLocalDir;
    QString currentRemotePath; /* 当前远程路径 */
    QIcon dirIcon;
    QIcon fileIcon;

    QJsonArray remoteFiles;
    QHash<QString, TransferTask> m_transferTasks;
};

#endif /* FILETRANSFERTOOL_H */
