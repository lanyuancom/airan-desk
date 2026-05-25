#include "control_window.h"
#include "file_transfer_window.h"
#include "ui/adaptive_ui.h"
#include "ui/app_title_bar.h"
#include "ui/control_window_view_helpers.h"
#include "ui_control_window.h"
#include "util/config_util.h"
#include "util/json_util.h"
#include "util/key_util.h"
#include <QScrollBar>
#include <QLayout>
#include <QApplication>
#include <QCoreApplication>
#include <QScreen>
#include <QRect>
#include <QStyle>
#include <QResizeEvent>
#include <QClipboard>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QBoxLayout>
#include <QGridLayout>
#include <QComboBox>
#include <QListView>
#include <QSpinBox>
#include <QFrame>
#include <QMouseEvent>
#include <QSettings>
#include <QPointer>
#include <QMetaObject>
#include <QSignalBlocker>
#include <QStylePainter>
#include <QStyleOption>
#include <QFontMetrics>
#include <QStandardItemModel>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QMessageBox>
#include <QToolButton>
#include <QEvent>

 /* namespace */

;

ControlWindow::ControlWindow(QString remoteId, QString remotePwdMd5, WsCli *_ws_cli, QWidget *parent)
    : QMainWindow(parent), isReceivedImg(false), windowSizeAdjusted(false),
      remote_id(remoteId), remote_pwd_md5(remotePwdMd5), m_rtc_ctl(remoteId, remotePwdMd5, false), m_ws(_ws_cli),
      m_floatingToolbar(nullptr), m_screenshotBtn(nullptr), m_switchScreenBtn(nullptr), m_remoteOperationBtn(nullptr), m_fileTransferBtn(nullptr), m_audioCaptureBtn(nullptr), m_diagnosticsBtn(nullptr), m_moreBtn(nullptr),
      m_statsLabel(nullptr), m_qualityModeCombo(nullptr), m_captureBackendCombo(nullptr), m_resolutionCombo(nullptr), m_displayModeCombo(nullptr), m_fitToWindow(false), m_fpsFrameCount(0), m_currentFps(0.0), m_currentKbps(0.0),
      m_streamMode(ConfigUtil->remote_quality),
      m_bitrateProfile(ConfigUtil->remote_bitrate_profile),
      m_networkPath(ConfigUtil->remote_network_path),
      m_audioMode(QStringLiteral("off")),
      m_draggingToolbar(false)
{
    initUI();
    initCLI();
    createFloatingToolbar();
    /* 初始化WebRtcCtl */
    emit initRtcCtl();
}

ControlWindow::~ControlWindow()
{
    LOG_DEBUG("ControlWindow destructor started");

    /* 先断开与 WebSocket/RTC 的互联，避免主窗口析构或 WebSocket 重连时仍投递到已关闭窗口。 */
    disconnect(&m_rtc_ctl, nullptr, m_ws, nullptr);
    if (m_ws)
        disconnect(m_ws, nullptr, &m_rtc_ctl, nullptr);
    disconnect(this, nullptr, &m_rtc_ctl, nullptr);
    disconnect(&m_rtc_ctl, nullptr, this, nullptr);

    /* 清理浮动工具栏及其按钮 */
    if (m_floatingToolbar)
    {
        /* 断开按钮信号连接 */
        if (m_screenshotBtn)
        {
            disconnect(m_screenshotBtn, nullptr, nullptr, nullptr);
        }
        if (m_switchScreenBtn)
        {
            disconnect(m_switchScreenBtn, nullptr, nullptr, nullptr);
        }
        if (m_remoteOperationBtn)
        {
            disconnect(m_remoteOperationBtn, nullptr, nullptr, nullptr);
        }
        if (m_fileTransferBtn)
        {
            disconnect(m_fileTransferBtn, nullptr, nullptr, nullptr);
        }
        if (m_audioCaptureBtn)
        {
            disconnect(m_audioCaptureBtn, nullptr, nullptr, nullptr);
        }
        if (m_qualityModeCombo)
        {
            disconnect(m_qualityModeCombo, nullptr, nullptr, nullptr);
        }
        if (m_resolutionCombo)
        {
            disconnect(m_resolutionCombo, nullptr, nullptr, nullptr);
        }
        if (m_captureBackendCombo)
        {
            disconnect(m_captureBackendCombo, nullptr, nullptr, nullptr);
        }
        if (m_displayModeCombo)
        {
            disconnect(m_displayModeCombo, nullptr, nullptr, nullptr);
        }

        m_floatingToolbar->hide();
        m_floatingToolbar->deleteLater();
        m_floatingToolbar = nullptr;
    }

    /* 必须在 WebRtcCtl 所属线程内先释放 PeerConnection/DataChannel/Timer。 */
    /* Win7/Win32 下对象仍归属已停止工作线程，随后在 GUI 线程析构时更容易崩溃。 */
    if (m_rtc_ctl_thread.isRunning())
    {
        QMetaObject::invokeMethod(&m_rtc_ctl, "shutdown", Qt::BlockingQueuedConnection);
    }

    /* 停止并清理WebRTC控制线程 */
    STOP_OBJ_THREAD(m_rtc_ctl_thread);

    disconnect();

    LOG_DEBUG("ControlWindow destructor finished");
    delete ui;
}

void ControlWindow::initUI()
{
    ui = new Ui::ControlWindow();
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose); /* 关闭时自动触发deleteLater() */

    /* 支持用户调整大小/最大化，视频在窗口内等比例缩放显示。 */
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(320, 240);

    setWindowTitle(tr("远程：%1").arg(remote_id));
    setMenuWidget(new AppTitleBar(this, true, true, this));

    /* 设置初始窗口大小（将在收到第一帧视频时自动调整） */
    resize(800, 600);

    /* 使用传统QLabel渲染 */
    label.setText(tr("正在连接..."));
    UiAdaptive::applyAdaptiveWindowSize(this, QSize(800, 600), QSize(320, 240));
    label.setAlignment(Qt::AlignCenter);
    label.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    /* 设置文字颜色为白色，在黑色背景上可见 */
    label.setStyleSheet("QLabel { background: black; border: none; margin: 0px; padding: 0px; color: white; font-size: 16px; }");

    /* 将QLabel设置为滚动区域的子部件 */
    scrollArea.setWidget(&label);
    scrollArea.setWidgetResizable(false);

    LOG_INFO("Initialized with QLabel video rendering, window size will auto-adjust to video");

    /* 禁用滚动条作为默认设置（当视频适合屏幕时） */
    scrollArea.setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea.setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    /* 设置Chrome风格的滚动条样式（备用，当需要时启用） */
    scrollArea.setStyleSheet(
        "QScrollArea {"
        "    border: none;"
        "    background: black;" /* 确保背景是黑色而不是白色 */
        "    margin: 0px;"
        "    padding: 0px;"
        "}"
        "QScrollArea > QWidget > QWidget {"
        "    background: black;" /* 确保内部widget也是黑色背景 */
        "}"
        "QScrollBar:vertical {"
        "    background: rgba(0,0,0,0);"
        "    width: 8px;"
        "    border-radius: 4px;"
        "}"
        "QScrollBar::handle:vertical {"
        "    background: rgba(128,128,128,0.5);"
        "    border-radius: 4px;"
        "    min-height: 20px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "    background: rgba(128,128,128,0.8);"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "    border: none;"
        "    background: none;"
        "    height: 0px;"
        "}"
        "QScrollBar:horizontal {"
        "    background: rgba(0,0,0,0);"
        "    height: 8px;"
        "    border-radius: 4px;"
        "}"
        "QScrollBar::handle:horizontal {"
        "    background: rgba(128,128,128,0.5);"
        "    border-radius: 4px;"
        "    min-width: 20px;"
        "}"
        "QScrollBar::handle:horizontal:hover {"
        "    background: rgba(128,128,128,0.8);"
        "}"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
        "    border: none;"
        "    background: none;"
        "    width: 0px;"
        "}");

    scrollArea.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    scrollArea.setAlignment(Qt::AlignCenter);
    /* 确保滚动区域没有内边距和边框 */
    scrollArea.setContentsMargins(0, 0, 0, 0);
    scrollArea.setFrameShape(QFrame::NoFrame); /* 去除边框 */
    scrollArea.setLineWidth(0);
    scrollArea.setMidLineWidth(0);

    /* 确保label也没有边框和内边距 */
    label.setContentsMargins(0, 0, 0, 0);
    /* 注释掉原来的样式设置，因为上面已经设置了包含颜色的完整样式 */
    /* label.setStyleSheet("QLabel { background: black; border: none; margin: 0px; padding: 0px; }"); */

    m_centralHost = new QWidget(this);
    auto *centralLayout = new QHBoxLayout(m_centralHost);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(&scrollArea, 1);

    this->setCentralWidget(m_centralHost);
    createAndroidNavigationPanel();
    if (m_androidNavHost)
        centralLayout->addWidget(m_androidNavHost);

    /* 确保主窗口也没有额外的边距 */
    this->setContentsMargins(0, 0, 0, 0);
    this->centralWidget()->setContentsMargins(0, 0, 0, 0);
}

void ControlWindow::initCLI()
{
    connect(&m_rtc_ctl, &WebRtcCtl::sendWsCliBinaryMsg, m_ws, &WsCli::sendWsCliBinaryMsg);
    connect(&m_rtc_ctl, &WebRtcCtl::sendWsCliTextMsg, m_ws, &WsCli::sendWsCliTextMsg);
    connect(m_ws, &WsCli::onWsCliRecvBinaryMsg, &m_rtc_ctl, &WebRtcCtl::onWsCliRecvBinaryMsg);
    connect(m_ws, &WsCli::onWsCliRecvTextMsg, &m_rtc_ctl, &WebRtcCtl::onWsCliRecvTextMsg);

    /* 配置rtc工作逻辑 */
    connect(this, &ControlWindow::initRtcCtl, &m_rtc_ctl, &WebRtcCtl::init);
    connect(this, &ControlWindow::sendMsg2InputChannel, &m_rtc_ctl, &WebRtcCtl::inputChannelSendMsg);

    connect(&m_rtc_ctl, &WebRtcCtl::videoFrameDecoded, this, &ControlWindow::updateImg);
    connect(&m_rtc_ctl, &WebRtcCtl::videoStatsUpdated, this, &ControlWindow::updateVideoStats);
    connect(&m_rtc_ctl, &WebRtcCtl::remoteCaptureBackendsChanged, this, &ControlWindow::onRemoteCaptureBackendsChanged);
    connect(&m_rtc_ctl, &WebRtcCtl::remoteStreamModeChanged, this, &ControlWindow::onRemoteStreamModeChanged);
    connect(&m_rtc_ctl, &WebRtcCtl::networkPathStateChanged, this, &ControlWindow::onNetworkPathStateChanged);
    connect(&m_rtc_ctl, &WebRtcCtl::remoteEncoderChanged, this, &ControlWindow::onRemoteEncoderChanged);
    connect(&m_rtc_ctl, &WebRtcCtl::desktopStateChanged, this, &ControlWindow::onRemoteDesktopStateChanged);
    connect(&m_rtc_ctl, &WebRtcCtl::remoteOsChanged, this, &ControlWindow::onRemoteOsChanged);
    connect(&m_rtc_ctl, &WebRtcCtl::runtimeDiagnosticsUpdated, this, &ControlWindow::onRuntimeDiagnosticsUpdated);

    m_rtc_ctl_thread.setObjectName("ControlWindow-WebRtcCtlThread");
    m_rtc_ctl.moveToThread(&m_rtc_ctl_thread);
    m_rtc_ctl_thread.start();
}

void ControlWindow::createAndroidNavigationPanel()
{
    if (!m_centralHost)
        return;

    if (m_androidNavPanel)
        return;

    m_androidNavHost = new QWidget(m_centralHost);
    m_androidNavHost->setFixedWidth(152);
    m_androidNavHost->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    auto *hostLayout = new QVBoxLayout(m_androidNavHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(0);

    m_androidNavPanel = new QFrame(m_androidNavHost);
    m_androidNavPanel->setObjectName(QStringLiteral("androidNavPanel"));
    m_androidNavPanel->setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
    m_androidNavPanel->setStyleSheet(
        "#androidNavPanel { background-color: rgba(38,38,38,242); border: 1px solid rgba(80,80,80,180); border-radius: 8px; }"
        "#androidNavPanel QPushButton { background-color: rgba(60,60,60,210); border: 1px solid rgba(105,105,105,170); border-radius: 4px; color: white; padding: 6px 10px; margin: 0px; font-size: 12px; }"
        "#androidNavPanel QPushButton:hover { background-color: rgba(82,82,82,230); border: 1px solid rgba(135,135,135,200); }"
        "#androidNavPanel QPushButton:pressed { background-color: rgba(48,48,48,245); }");
    m_androidNavPanel->installEventFilter(this);

    auto *panelLayout = new QVBoxLayout(m_androidNavPanel);
    panelLayout->setContentsMargins(10, 10, 10, 10);
    panelLayout->setSpacing(6);

    auto *title = new QLabel(tr("Android"), m_androidNavPanel);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("QLabel { color: rgba(255,255,255,220); background: transparent; border: none; font-size: 11px; }");
    title->installEventFilter(this);
    panelLayout->addWidget(title);

    auto addNavButton = [this, panelLayout](QPushButton *&button, const QString &text, const QString &action, const QString &tip)
    {
        button = new QPushButton(text, m_androidNavPanel);
        button->setToolTip(tip);
        fitControlButtonWidthToText(button);
        connect(button, &QPushButton::clicked, this, [this, action]()
                { sendAndroidNavigation(action); });
        panelLayout->addWidget(button);
    };

    addNavButton(m_androidBackBtn, tr("返回"), QStringLiteral("back"), tr("Android 返回"));
    addNavButton(m_androidHomeBtn, tr("主页"), QStringLiteral("home"), tr("Android 主页"));
    addNavButton(m_androidMenuBtn, tr("菜单"), QStringLiteral("menu"), tr("Android 菜单"));
    addNavButton(m_androidRecentsBtn, tr("最近"), QStringLiteral("recents"), tr("Android 最近任务"));
    panelLayout->addStretch(1);

    m_androidNavHost->setFixedWidth(0);
    m_androidNavHost->hide();
    constrainAndroidNavigationPanel();
}

void ControlWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateScaledPixmap();
    updateToolbarPosition();
    constrainAndroidNavigationPanel();
}

QPointF ControlWindow::getNormPoint(const QPoint &pos)
{
    if (m_sourcePixmap.isNull() || m_videoDisplayRect.isEmpty())
        return QPointF(-1.0, -1.0);

    const QPoint labelPos = label.mapFrom(scrollArea.viewport(), scrollArea.viewport()->mapFrom(this, pos));
    if (!m_videoDisplayRect.contains(labelPos))
        return QPointF(-1.0, -1.0);

    const qreal x = (labelPos.x() - m_videoDisplayRect.x()) / static_cast<qreal>(m_videoDisplayRect.width());
    const qreal y = (labelPos.y() - m_videoDisplayRect.y()) / static_cast<qreal>(m_videoDisplayRect.height());
    return QPointF(qBound(0.0, x, 1.0), qBound(0.0, y, 1.0));
}

bool ControlWindow::isValidNormPoint(const QPointF &pos) const
{
    return pos.x() >= 0.0 && pos.x() <= 1.0 && pos.y() >= 0.0 && pos.y() <= 1.0;
}

bool ControlWindow::eventFilter(QObject *watched, QEvent *event)
{
    const bool toolbarDragTarget = watched == m_floatingToolbar ||
                                   watched == m_statsLabel ||
                                   watched == m_toolbarButtonRow ||
                                   watched == m_toolbarOptionsPanel;
    if (toolbarDragTarget && m_floatingToolbar)
    {
        if (event->type() == QEvent::MouseButtonPress)
        {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton)
            {
                m_draggingToolbar = true;
                m_dragStartPosition = mouseEvent->globalPos();
                m_toolbarOffset = mouseEvent->globalPos() - m_floatingToolbar->mapToGlobal(QPoint(0, 0));
                mouseEvent->accept();
                return true;
            }
        }
        else if (event->type() == QEvent::MouseMove && m_draggingToolbar)
        {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            QWidget *toolbarParent = m_floatingToolbar->parentWidget();
            if (!toolbarParent)
                toolbarParent = this;

            QPoint target = toolbarParent->mapFromGlobal(mouseEvent->globalPos() - m_toolbarOffset);
            const int maxX = qMax(0, toolbarParent->width() - m_floatingToolbar->width());
            const int maxY = qMax(0, toolbarParent->height() - m_floatingToolbar->height());
            target.setX(qBound(0, target.x(), maxX));
            target.setY(qBound(0, target.y(), maxY));
            m_floatingToolbar->move(target);
            m_toolbarUserMoved = true;
            mouseEvent->accept();
            return true;
        }
        else if (event->type() == QEvent::MouseButtonRelease && m_draggingToolbar)
        {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton)
            {
                m_draggingToolbar = false;
                m_toolbarUserMoved = true;
                mouseEvent->accept();
                return true;
            }
        }
    }

    const bool navDragTarget = watched == m_androidNavPanel ||
                               (m_androidNavPanel && qobject_cast<QWidget *>(watched) &&
                                qobject_cast<QWidget *>(watched)->parentWidget() == m_androidNavPanel);

    if (!navDragTarget || !m_androidNavPanel || !m_androidNavHost)
        return QMainWindow::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress)
    {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton)
        {
            m_draggingAndroidNav = true;
            m_androidNavDragStart = mouseEvent->globalPos();
            m_androidNavStartPos = m_androidNavPanel->pos();
            mouseEvent->accept();
            return true;
        }
    }
    else if (event->type() == QEvent::MouseMove && m_draggingAndroidNav)
    {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const QPoint delta = mouseEvent->globalPos() - m_androidNavDragStart;
        QPoint target = m_androidNavStartPos + delta;
        const int maxX = qMax(0, m_androidNavHost->width() - m_androidNavPanel->width());
        const int maxY = qMax(0, m_androidNavHost->height() - m_androidNavPanel->height());
        target.setX(qBound(0, target.x(), maxX));
        target.setY(qBound(0, target.y(), maxY));
        m_androidNavPanel->move(target);
        mouseEvent->accept();
        return true;
    }
    else if (event->type() == QEvent::MouseButtonRelease && m_draggingAndroidNav)
    {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton)
        {
            m_draggingAndroidNav = false;
            mouseEvent->accept();
            return true;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}
