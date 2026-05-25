/* Split from control_window_view.cpp to keep ControlWindow UI responsibilities modular. */

#include "control_window.h"
#include "ui/control_window_view_helpers.h"

#include <QAction>
#include <QActionGroup>
#include <QBoxLayout>
#include <QFrame>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
    QAction *addCheckedAction(QMenu *menu, QActionGroup *group, const QString &text, const QString &data, bool checked = false)
    {
        QAction *action = menu->addAction(text);
        action->setCheckable(true);
        action->setData(data);
        action->setChecked(checked);
        if (group)
            group->addAction(action);
        return action;
    }

    int textWidth(const QFontMetrics &metrics, const QString &text)
    {
        return controlWindowTextWidth(metrics, text);
    }

    void fitButtonWidthToText(QPushButton *button)
    {
        fitControlButtonWidthToText(button);
    }
}

void ControlWindow::createFloatingToolbar()
{
    m_floatingToolbar = new QFrame(this);
    m_floatingToolbar->setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
    m_floatingToolbar->setStyleSheet(
        "QFrame { background-color: rgba(38,38,38,242); border: 1px solid rgba(80,80,80,180); border-radius: 8px; }"
        "QLabel { color: rgba(255,255,255,230); background: transparent; border: none; padding: 0px 4px; font-size: 10px; }"
        "QPushButton { background-color: rgba(60,60,60,210); border: 1px solid rgba(105,105,105,170); border-radius: 4px; color: white; padding: 5px 12px; margin: 0px; font-size: 12px; }"
        "QPushButton:hover { background-color: rgba(82,82,82,230); border: 1px solid rgba(135,135,135,200); }"
        "QPushButton:pressed { background-color: rgba(48,48,48,245); }"
        "QToolButton { background-color: rgba(60,60,60,210); border: 1px solid rgba(105,105,105,170); border-radius: 4px; color: white; padding: 5px 8px; margin: 0px; font-size: 12px; }"
        "QToolButton:hover { background-color: rgba(82,82,82,230); border: 1px solid rgba(135,135,135,200); }"
        "QToolButton:pressed, QToolButton:checked { background-color: rgba(120,48,65,235); border: 1px solid rgba(180,115,130,220); color: white; }"
        "QToolButton:disabled { background-color: rgba(45,45,45,180); border: 1px solid rgba(75,75,75,160); color: rgba(255,255,255,95); }"
        "QComboBox { background-color: rgba(60,60,60,210); border: 1px solid rgba(105,105,105,170); border-radius: 4px; color: white; padding: 5px 22px 5px 10px; margin: 0px; font-size: 12px; }"
        "QComboBox:hover { background-color: rgba(82,82,82,230); border: 1px solid rgba(135,135,135,200); }"
        "QComboBox::drop-down { border: none; background: transparent; width: 20px; }"
        "QComboBox QAbstractItemView { background-color: rgb(42,42,42); border: 1px solid rgba(120,120,120,220); color: white; padding: 4px; outline: 0; selection-background-color: rgba(70,125,220,230); selection-color: white; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(m_floatingToolbar);
    mainLayout->setSpacing(5);
    mainLayout->setContentsMargins(24, 7, 24, 8);

    m_statsLabel = new QLabel("FPS: -- | Kbps: -- | -x- | -- (--)", m_floatingToolbar);
    m_statsLabel->setToolTip(tr("实时显示解码帧率、接收码率、被控端视频分辨率和编码器信息"));
    m_statsLabel->setAlignment(Qt::AlignCenter);
    /* Calculate adaptive minimum width based on font metrics to support longer localized strings */
    QFontMetrics statsFm(m_statsLabel->font());
    int statsTextW = textWidth(statsFm, m_statsLabel->text());
    const int padding = 24;
    const int minAdaptive = qMax(220, statsTextW + padding);
    m_statsLabel->setMinimumWidth(minAdaptive);
    mainLayout->addWidget(m_statsLabel);

    m_toolbarButtonRow = new QWidget(m_floatingToolbar);
    m_toolbarButtonRow->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_toolbarButtonLayout = new QBoxLayout(QBoxLayout::LeftToRight, m_toolbarButtonRow);
    m_toolbarButtonLayout->setSizeConstraint(QLayout::SetFixedSize);
    m_toolbarButtonLayout->setSpacing(4);
    m_toolbarButtonLayout->setContentsMargins(0, 0, 0, 0);

    m_screenshotBtn = new QPushButton(tr("📸 截屏"), m_floatingToolbar);
    m_screenshotBtn->setToolTip(tr("截取当前窗口图像到剪切板"));
    fitButtonWidthToText(m_screenshotBtn);
    connect(m_screenshotBtn, &QPushButton::clicked, this, &ControlWindow::onScreenshotClicked);
    m_toolbarButtonLayout->addWidget(m_screenshotBtn);

    m_remoteOperationBtn = new QPushButton(tr("快捷"), m_floatingToolbar);
    m_remoteOperationBtn->setToolTip(tr("向被控端发送系统操作命令"));
    fitButtonWidthToText(m_remoteOperationBtn);
    connect(m_remoteOperationBtn, &QPushButton::clicked, this, &ControlWindow::onRemoteOperationTriggered);
    m_toolbarButtonLayout->addWidget(m_remoteOperationBtn);

    m_captureBackendRollbackTimer = new QTimer(this);
    m_captureBackendRollbackTimer->setSingleShot(true);
    connect(m_captureBackendRollbackTimer, &QTimer::timeout, this, &ControlWindow::rollbackCaptureBackendSelection);

    m_fileTransferBtn = new QPushButton(tr("📁 文件"), m_floatingToolbar);
    m_fileTransferBtn->setToolTip(tr("打开文件传输窗口"));
    fitButtonWidthToText(m_fileTransferBtn);
    connect(m_fileTransferBtn, &QPushButton::clicked, this, &ControlWindow::onFileTransferClicked);
    m_toolbarButtonLayout->addWidget(m_fileTransferBtn);

    m_audioCaptureBtn = new QPushButton(tr("音频关闭"), m_floatingToolbar);
    m_audioCaptureBtn->setToolTip(tr("选择远程音频模式"));
    fitButtonWidthToText(m_audioCaptureBtn);
    connect(m_audioCaptureBtn, &QPushButton::clicked, this, &ControlWindow::onAudioCaptureClicked);
    m_toolbarButtonLayout->addWidget(m_audioCaptureBtn);

    m_diagnosticsBtn = new QPushButton(tr("诊断"), m_floatingToolbar);
    m_diagnosticsBtn->setToolTip(tr("查看当前远程桌面运行时诊断"));
    fitButtonWidthToText(m_diagnosticsBtn);
    connect(m_diagnosticsBtn, &QPushButton::clicked, this, &ControlWindow::onDiagnosticsClicked);
    m_toolbarButtonLayout->addWidget(m_diagnosticsBtn);

    m_moreMenu = new QMenu(m_floatingToolbar);

    QMenu *channelMenu = m_moreMenu->addMenu(tr("通道"));
    m_channelActionGroup = new QActionGroup(this);
    m_channelActionGroup->setExclusive(true);
    addCheckedAction(channelMenu, m_channelActionGroup, tr("稳定"), "quality");
    addCheckedAction(channelMenu, m_channelActionGroup, tr("流畅"), "smooth", true);
    addCheckedAction(channelMenu, m_channelActionGroup, tr("兼容"), "compat");
    connect(m_channelActionGroup, &QActionGroup::triggered, this, &ControlWindow::onChannelModeSelected);

    QMenu *bitrateMenu = m_moreMenu->addMenu(tr("画质"));
    m_bitrateActionGroup = new QActionGroup(this);
    m_bitrateActionGroup->setExclusive(true);
    addCheckedAction(bitrateMenu, m_bitrateActionGroup, tr("低"), "low");
    addCheckedAction(bitrateMenu, m_bitrateActionGroup, tr("中"), "medium", true);
    addCheckedAction(bitrateMenu, m_bitrateActionGroup, tr("高"), "high");
    connect(m_bitrateActionGroup, &QActionGroup::triggered, this, &ControlWindow::onBitrateProfileSelected);

    QMenu *resolutionMenu = m_moreMenu->addMenu(tr("分辨率"));
    m_resolutionActionGroup = new QActionGroup(this);
    m_resolutionActionGroup->setExclusive(true);
    QAction *resolutionOriginal = resolutionMenu->addAction(tr("原始"));
    resolutionOriginal->setCheckable(true);
    resolutionOriginal->setChecked(true);
    resolutionOriginal->setData(QSize(0, 0));
    m_resolutionActionGroup->addAction(resolutionOriginal);

    QAction *resolution720p = resolutionMenu->addAction("1280x720");
    resolution720p->setCheckable(true);
    resolution720p->setData(QSize(1280, 720));
    m_resolutionActionGroup->addAction(resolution720p);

    QAction *resolution1080p = resolutionMenu->addAction("1920x1080");
    resolution1080p->setCheckable(true);
    resolution1080p->setData(QSize(1920, 1080));
    m_resolutionActionGroup->addAction(resolution1080p);
    connect(m_resolutionActionGroup, &QActionGroup::triggered, this, &ControlWindow::onResolutionSelected);

    QMenu *captureMenu = m_moreMenu->addMenu(tr("采集方式"));
    m_captureActionGroup = new QActionGroup(this);
    m_captureActionGroup->setExclusive(true);
    addCheckedAction(captureMenu, m_captureActionGroup, "GPU", "wgc", true);
    addCheckedAction(captureMenu, m_captureActionGroup, "CPU", "qt");
    connect(m_captureActionGroup, &QActionGroup::triggered, this, &ControlWindow::onCaptureBackendSelected);

    QMenu *networkMenu = m_moreMenu->addMenu(tr("网络路径"));
    m_networkActionGroup = new QActionGroup(this);
    m_networkActionGroup->setExclusive(true);
    addCheckedAction(networkMenu, m_networkActionGroup, tr("自动"), "auto", true);
    addCheckedAction(networkMenu, m_networkActionGroup, tr("直连"), "direct");
    addCheckedAction(networkMenu, m_networkActionGroup, tr("UDP中继"), "turn_udp");
    addCheckedAction(networkMenu, m_networkActionGroup, tr("TCP中继"), "turn_tcp");
    connect(m_networkActionGroup, &QActionGroup::triggered, this, &ControlWindow::onNetworkPathSelected);

    QMenu *displayMenu = m_moreMenu->addMenu(tr("显示"));
    m_displayActionGroup = new QActionGroup(this);
    m_displayActionGroup->setExclusive(true);
    addCheckedAction(displayMenu, m_displayActionGroup, "1:1", "actual");
    addCheckedAction(displayMenu, m_displayActionGroup, tr("适应窗口"), "fit", true);
    connect(m_displayActionGroup, &QActionGroup::triggered, this, &ControlWindow::onDisplayModeSelected);

    m_moreBtn = new QPushButton(tr("更多"), m_floatingToolbar);
    m_moreBtn->setToolTip(tr("更多画面、网络和显示选项"));
    fitButtonWidthToText(m_moreBtn);
    connect(m_moreBtn, &QPushButton::clicked, this, &ControlWindow::onMoreMenuRequested);
    m_toolbarButtonLayout->addWidget(m_moreBtn);

    m_toolbarOptionsPanel = new QWidget(m_floatingToolbar);
    m_toolbarOptionsPanel->setVisible(false);
    auto *optionsLayout = new QVBoxLayout(m_toolbarOptionsPanel);
    optionsLayout->setContentsMargins(0, 4, 0, 0);
    optionsLayout->setSpacing(5);

    auto addTopLevelMenuButton = [this, optionsLayout](const QString &title, QMenu *menu, QActionGroup *group)
    {
        if (!menu || !group)
            return;

        auto *button = new QToolButton(m_toolbarOptionsPanel);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setPopupMode(QToolButton::InstantPopup);
        button->setMenu(menu);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(38);
        button->setToolTip(title);

        auto updateButton = [button, title, group]()
        {
            QAction *checked = group->checkedAction();
            QString current = checked ? checked->text() : QString();
            current.remove(QChar('&'));
            button->setText(current.isEmpty() ? title : QStringLiteral("%1\n%2").arg(title, current));
        };

        connect(group, &QActionGroup::triggered, button, [updateButton](QAction *)
                { updateButton(); });
        for (QAction *action : group->actions())
        {
            connect(action, &QAction::changed, button, updateButton);
        }
        updateButton();

        optionsLayout->addWidget(button);
        m_sideMenuButtons.append(button);
    };

    addTopLevelMenuButton(tr("通道"), channelMenu, m_channelActionGroup);
    addTopLevelMenuButton(tr("画质"), bitrateMenu, m_bitrateActionGroup);
    addTopLevelMenuButton(tr("分辨率"), resolutionMenu, m_resolutionActionGroup);
    addTopLevelMenuButton(tr("采集方式"), captureMenu, m_captureActionGroup);
    addTopLevelMenuButton(tr("网络路径"), networkMenu, m_networkActionGroup);
    addTopLevelMenuButton(tr("显示"), displayMenu, m_displayActionGroup);

    m_fitToWindow = true;
    onDisplayModeSelected(m_displayActionGroup->checkedAction());

    mainLayout->addWidget(m_toolbarButtonRow, 0, Qt::AlignHCenter);
    mainLayout->addWidget(m_toolbarOptionsPanel);

    m_floatingToolbar->setMouseTracking(true);
    m_floatingToolbar->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_floatingToolbar->installEventFilter(this);
    m_statsLabel->installEventFilter(this);
    m_toolbarButtonRow->installEventFilter(this);
    m_toolbarOptionsPanel->installEventFilter(this);

    updateToolbarPosition();
    m_floatingToolbar->raise();
    m_floatingToolbar->show();
}
