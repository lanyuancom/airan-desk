#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QWidget>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QCloseEvent>
#include <QFont>
#include <QRect>
#include "websocket/ws_cli.h"
#include "webrtc/webrtc_cli.h"

class QPushButton;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QRadioButton;
class QWidget;
class AppTitleBar;
class QCloseEvent;
class SettingsWindow;

namespace Ui
{
    class MainWindow;
}
/**
 * @brief The MainWindow class  程序主窗口
 */
class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    /* 设置ui组件 */
    void initUI();
    /* 初始化websocket连接 */
    void initCli();
    /* 连接到文件传输 */
    void connFileMgr(const QString &remote_id, const QString &remote_pwd_md5);
    /* 连接到远程桌面窗口 */
    void connDesktopMgr(const QString &remote_id, const QString &remote_pwd_md5);
    void connTerminalMgr(const QString &remote_id, const QString &remote_pwd_md5);

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
#endif
signals:
    void closeWsCli();
    void initWsCli(const QString &url, quint64 heart_interval_ms);
    void resetWsCliUrl(const QString &url);
    void sendWsCliTextMsg(const QString &message);
    void sendWsCliBinaryMsg(const QByteArray &message);
    void initRtcCli();
private slots:
    /* 当连接按钮被触发 */
    void on_btn_conn_clicked();
    /* 当密码更新按钮被触发 */
    void on_local_pwd_change_clicked();
    /* 当分享按钮被触发 */
    void on_local_share_clicked();
    /* websocket连接成功 */
    void onWsCliConnected();
    /* websocket断开连接 */
    void onWsCliDisconnected();
    /* websocket重连状态更新 */
    void onWsCliReconnectStatus(const QString &status, int phase, int attempt, int nextDelaySeconds);
    /* websocket接收到文本消息 */
    void onWsCliRecvTextMsg(const QString &message);
    /* websocket接收到二进制消息 */
    void onWsCliRecvBinaryMsg(const QByteArray &message);

    void onDestroyWebRtcCli();
    void showWindowFromTray();
    void openSettingsFromTray();
    void showRuntimeDiagnostics();
    void quitFromTray();

private:
    QString buildWsUrl() const;
    void handleDeviceIdConflict(const QJsonObject &object);
    QString localizedErrorMessage(const QString &message) const;
    void cleanupWebRtcCliSessions();
    void initTray();
    void bindUiObjects();
    void applyMainScale();
    void layoutMainContent();
    bool tryFillRemoteFieldsFromShareText(const QString &text);
    int scaled(int value) const;
    QRect scaledRect(int x, int y, int width, int height) const;
    QFont scaledFont(double pointSize, bool bold = false) const;
    void updateDesktopStateForSessions(bool locked, const QString &message);

    QWidget *m_content{nullptr};
    AppTitleBar *m_titleBar{nullptr};
    QLabel *m_allowControlLabel{nullptr};
    QLabel *m_localIdLabel{nullptr};
    QLabel *m_localPwdLabel{nullptr};
    QLabel *m_remoteControlLabel{nullptr};
    QLabel *m_remoteIdLabel{nullptr};
    QLabel *m_remotePwdLabel{nullptr};
    QLabel *m_wsConnectStatus{nullptr};
    QLabel *m_versionLabel{nullptr};
    QLineEdit *m_localIdEdit{nullptr};
    QLineEdit *m_localPwdEdit{nullptr};
    QLineEdit *m_remoteIdEdit{nullptr};
    QLineEdit *m_remotePwdEdit{nullptr};
    QWidget *m_localIdBorder{nullptr};
    QWidget *m_localPwdBorder{nullptr};
    QWidget *m_remoteIdBorder{nullptr};
    QWidget *m_remotePwdBorder{nullptr};
    QRadioButton *m_remoteDesktopRadio{nullptr};
    QRadioButton *m_remoteFileRadio{nullptr};
    QRadioButton *m_remoteTerminalRadio{nullptr};
    QPushButton *m_connectButton{nullptr};
    QPushButton *m_localPwdChangeButton{nullptr};
    QPushButton *m_localShareButton{nullptr};
    QPushButton *m_settingsButton{nullptr};
    QWidget *m_connectDivider{nullptr};
    QWidget *m_localPwdChangeDivider{nullptr};
    QWidget *m_localShareDivider{nullptr};

    QString windowTitle;
    QString textToCopy;
    WsCli m_ws;
    QThread m_ws_thread;
    QHash<WebRtcCli *, QThread *> m_rtcCliSessions;
    bool isCaptureing;
    bool m_remoteShareParsing{false};
    bool m_desktopLocked{false};
    QSystemTrayIcon *m_trayIcon{nullptr};
    QMenu *m_trayMenu{nullptr};
    QAction *m_trayOpenAction{nullptr};
    QAction *m_traySettingsAction{nullptr};
    QAction *m_trayDiagnosticsAction{nullptr};
    QAction *m_trayQuitAction{nullptr};
    SettingsWindow *m_settingsWindow{nullptr};
    double m_uiScale{1.0};
    Ui::MainWindow *ui{nullptr};
};

#endif /* MAIN_WINDOW_H */
