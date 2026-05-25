#ifndef CONTROL_WINDOW_H
#define CONTROL_WINDOW_H

#include <QMainWindow>
#include <QWheelEvent>
#include <QScrollArea>
#include <QTimer>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QComboBox>
#include <QSpinBox>
#include <QFrame>
#include <QApplication>
#include <QClipboard>
#include <QButtonGroup>
#include <QElapsedTimer>
#include <QPixmap>
#include <QActionGroup>
#include <QMenu>
#include <QList>
#include "common/constant.h"
#include "webrtc/webrtc_ctl.h"
#include "websocket/ws_cli.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
    class ControlWindow;
}
class QKeyEvent;
class QMouseEvent;
class QResizeEvent;
class QEvent;
class QBoxLayout;
class QToolButton;

class ControlWindow : public QMainWindow
{
    Q_OBJECT
public:
    ControlWindow(QString remoteId, QString remotePwdMd5, WsCli *_ws_cli, QWidget *parent = nullptr);
    ~ControlWindow();
    void initUI();
    void initCLI();

    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    /* 获取归一化坐标 */
    QPointF getNormPoint(const QPoint &pos);
    bool isValidNormPoint(const QPointF &pos) const;

    /* 浮动工具栏相关方法 */
    void createFloatingToolbar();
    void createAndroidNavigationPanel();
    void updateToolbarPosition();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool isReceivedImg;      /* 是否接收到图片 */
    bool windowSizeAdjusted; /* 标记窗口大小是否已经根据视频调整过 */
    QScrollArea scrollArea;

    QLabel label;
    QPixmap m_sourcePixmap;
    QRect m_videoDisplayRect;

    QString remote_id;
    QString remote_pwd_md5;
    WebRtcCtl m_rtc_ctl;
    WsCli *m_ws;
    QThread m_rtc_ctl_thread;

    /* 浮动工具栏 */
    QFrame *m_floatingToolbar;
    QPushButton *m_screenshotBtn;
    QPushButton *m_switchScreenBtn;
    QPushButton *m_remoteOperationBtn;
    QPushButton *m_fileTransferBtn;
    QPushButton *m_audioCaptureBtn{nullptr};
    QPushButton *m_diagnosticsBtn{nullptr};
    QPushButton *m_androidBackBtn{nullptr};
    QPushButton *m_androidHomeBtn{nullptr};
    QPushButton *m_androidMenuBtn{nullptr};
    QPushButton *m_androidRecentsBtn{nullptr};
    QPushButton *m_moreBtn;
    QWidget *m_centralHost{nullptr};
    QWidget *m_androidNavHost{nullptr};
    QFrame *m_androidNavPanel{nullptr};
    QWidget *m_toolbarButtonRow{nullptr};
    QWidget *m_toolbarOptionsPanel{nullptr};
    QBoxLayout *m_toolbarButtonLayout{nullptr};
    QList<QToolButton *> m_sideMenuButtons;
    QLabel *m_statsLabel;
    QComboBox *m_qualityModeCombo;
    QComboBox *m_captureBackendCombo;
    QComboBox *m_resolutionCombo;
    QComboBox *m_displayModeCombo;
    QMenu *m_moreMenu{nullptr};
    QActionGroup *m_channelActionGroup{nullptr};
    QActionGroup *m_bitrateActionGroup{nullptr};
    QActionGroup *m_resolutionActionGroup{nullptr};
    QActionGroup *m_captureActionGroup{nullptr};
    QActionGroup *m_networkActionGroup{nullptr};
    QActionGroup *m_displayActionGroup{nullptr};
    bool m_fitToWindow;
    QElapsedTimer m_fpsTimer;
    int m_fpsFrameCount;
    double m_currentFps;
    double m_currentKbps;
    QSize m_remoteResolution;
    QString m_confirmedCaptureBackend;
    QString m_pendingCaptureBackend;
    QTimer *m_captureBackendRollbackTimer;
    QString m_remoteEncoderName;
    QString m_remoteEncoderType;
    bool m_remoteEncoderZeroCopy{false};
    QString m_streamMode{QStringLiteral("smooth")};
    QString m_bitrateProfile{QStringLiteral("medium")};
    QString m_networkPath{QStringLiteral("auto")};
    bool m_remoteDesktopLocked{false};
    bool m_androidNavigationVisible{false};
    bool m_audioCaptureEnabled{false};
    QString m_audioMode{QStringLiteral("off")};
    QString m_remoteOsName;
    QString m_runtimeDiagnostics;

    /* 工具栏拖拽相关 */
    bool m_draggingToolbar;
    QPoint m_dragStartPosition;
    QPoint m_toolbarOffset;
    bool m_toolbarInSidePanel{false};
    bool m_toolbarUserMoved{false};
    bool m_draggingAndroidNav{false};
    QPoint m_androidNavDragStart;
    QPoint m_androidNavStartPos;
    QSize m_windowSize; /* 用于存储窗口大小 */
    Ui::ControlWindow *ui{nullptr};
signals:
    void sendMsg2InputChannel(const rtc::message_variant &data);
    void initRtcCtl();
public slots:
    void updateImg(const QImage &img);
    void updateVideoStats(double kbps, const QSize &resolution);

    /* 工具栏按钮槽函数 */
    void onScreenshotClicked();
    void onSwitchScreenClicked();
    void onRemoteOperationTriggered();
    void onFileTransferClicked();
    void onAudioCaptureClicked();
    void onDiagnosticsClicked();
    void onQualityModeChanged(int index);
    void onCaptureBackendChanged(int index);
    void onTransferResolutionChanged(int index);
    void onDisplayModeChanged(int index);
    void onMoreMenuRequested();
    void onChannelModeSelected(QAction *action);
    void onBitrateProfileSelected(QAction *action);
    void onResolutionSelected(QAction *action);
    void onCaptureBackendSelected(QAction *action);
    void onNetworkPathSelected(QAction *action);
    void onDisplayModeSelected(QAction *action);
    void onRemoteStreamModeChanged(const QString &mode);
    void onRemoteCaptureBackendsChanged(const QStringList &backends, const QString &currentBackend);
    void onNetworkPathStateChanged(const QStringList &availablePaths, const QString &selectedPath, const QString &requestedPath);
    void onRemoteEncoderChanged(const QString &encoderName, const QString &encoderType, bool zeroCopy);
    void onRemoteDesktopStateChanged(bool locked, const QString &message);
    void onRemoteOsChanged(const QString &osName);
    void onRuntimeDiagnosticsUpdated(const QString &diagnostics);

private slots:
    void refreshStatsLabel();
    void restoreScreenshotButtonText();
    void rollbackCaptureBackendSelection();
    void adjustWindowSizeToVideo(const QSize &videoSize); /* 根据视频尺寸调整窗口大小 */
    void updateScaledPixmap();
    void sendAndroidNavigation(const QString &action);
    void setAndroidNavigationVisible(bool visible);
    void constrainAndroidNavigationPanel();
    bool isRemotePortrait() const;
    bool shouldPlaceToolbarInSidePanel() const;
    void applyToolbarLayoutMode(bool sidePanelMode);
    void updateAndroidSidePanelWidth();
};
#endif /* CONTROL_WINDOW_H */
