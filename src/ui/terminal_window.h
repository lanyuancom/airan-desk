#ifndef TERMINAL_WINDOW_H
#define TERMINAL_WINDOW_H

#include <QFile>
#include <QThread>
#include <QWidget>
#include "webrtc/webrtc_ctl.h"
#include "websocket/ws_cli.h"

class NativeTerminalWidget;
class TerminalFilePanel;
class QSplitter;
class QCheckBox;
class QLabel;

namespace Ui
{
    class TerminalWindow;
}

class TerminalWindow : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalWindow(QString remoteId, QString remotePwdMd5, WsCli *wsCli, QWidget *parent = nullptr);
    ~TerminalWindow();

private:
    void initUI();
    void initCLI();
    void tryStartTerminal();
    void updateTerminalTitle(const QString &title);
    QByteArray filterTerminalOutput(const QByteArray &data);
    QByteArray normalizePipeTerminalOutput(const QByteArray &data);
    void sendTerminalResize(const QSize &gridSize);
    void sendTerminalInput(const QByteArray &data);
    void requestFileList(const QString &path);
    void requestDownload(const QString &remotePath, const QString &localPath, bool isDirectory);
    void requestUpload(const QString &localPath, const QString &remotePath, bool isDirectory);
    void injectPathTracking();
    void setTerminalAutoSave(bool enabled);
    bool openTerminalLogFile();
    void closeTerminalLogFile();
    void appendTerminalLog(const QByteArray &data);
    QString plainTextFromTerminalOutput(const QByteArray &data) const;
    void writeTerminalLogLine(const QString &line);
    QString defaultTerminalLogPath() const;
    void updateTerminalLogUi();

private slots:
    void onFileTextChannelOpened();
    void onTerminalOutput(const QByteArray &data);
    void onTerminalInfo(const QString &osName, const QString &shellPath, const QString &mode, bool pathTracking);
    void onTerminalClosed(int exitCode);
    void onTerminalError(const QString &message);
    void onAutoSaveLogToggled(bool checked);

private:
    QString m_remoteId;
    QString m_remotePwdMd5;
    WebRtcCtl m_rtcCtl;
    WsCli *m_ws = nullptr;
    QThread m_rtcThread;
    NativeTerminalWidget *m_terminal = nullptr;
    TerminalFilePanel *m_filePanel = nullptr;
    QCheckBox *m_autoSaveLogCheck = nullptr;
    QLabel *m_autoSaveLogPathLabel = nullptr;
    QFile m_terminalLogFile;
    QString m_terminalLogPath;
    QString m_terminalLogPendingLine;
    QByteArray m_terminalFilterPending;
    bool m_lastPipeOutputWasCr = false;
    bool m_channelReady = false;
    bool m_started = false;
    bool m_terminalLogEnabled = false;
    QString m_remoteOs;
    QString m_remoteShell;
    QString m_remoteTerminalMode;
    bool m_remotePathTracking = true;
    bool m_filterWindowsPromptEcho = false;
    Ui::TerminalWindow *ui{nullptr};

signals:
    void initRtcCtl();
    void fileTextChannelSendMsg(const rtc::message_variant &data);
    void uploadFile2CLI(const QString &ctlPath, const QString &cliPath, const QString &transferId);
};

#endif /* TERMINAL_WINDOW_H */
