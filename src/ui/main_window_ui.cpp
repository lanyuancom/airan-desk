#include "main_window.h"
#include "common/constant.h"
#include "ui/app_title_bar.h"
#include "ui/adaptive_ui.h"
#include "ui/password_line_edit_util.h"
#include "ui_main_window.h"
#include <QApplication>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSignalBlocker>

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#include <windows.h>
#include <wtsapi32.h>
#endif

namespace
{
    QIcon makeSettingsIcon()
    {
        QPixmap pixmap(32, 32);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.translate(16, 16);

        QPen pen(QColor(131, 193, 224), 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        for (int i = 0; i < 8; ++i)
        {
            painter.save();
            painter.rotate(i * 45.0);
            painter.drawLine(QPointF(0, -12), QPointF(0, -9));
            painter.restore();
        }

        painter.drawEllipse(QPointF(0, 0), 9.0, 9.0);
        painter.drawEllipse(QPointF(0, 0), 3.2, 3.2);
        return QIcon(pixmap);
    }

    QString firstCapture(const QString &text, const QStringList &patterns)
    {
        for (const QString &pattern : patterns)
        {
            QRegularExpression regex(pattern, QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch match = regex.match(text);
            if (match.hasMatch())
                return match.captured(1).trimmed();
        }
        return QString();
    }
}

void MainWindow::initUI()
{
    ui = new Ui::MainWindow();
    ui->setupUi(this);
    setObjectName(QStringLiteral("MainWindow"));
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowTitle(windowTitle);
    setWindowIcon(qApp->windowIcon());
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    if (ui->rootLayout)
        ui->rootLayout->setContentsMargins(2, 2, 2, 2);

    m_titleBar = new AppTitleBar(this, true, false, this);
    m_titleBar->setResizeAspectRatio(QSize(800, 638));
    if (ui->titleBarLayout)
        ui->titleBarLayout->addWidget(m_titleBar);

    m_content = ui->mainContent;
    bindUiObjects();
    applyMainScale();

    m_localIdEdit->setText(ConfigUtil->local_id);
    m_localPwdEdit->setText(ConfigUtil->getLocalPwd());
    m_localIdEdit->setReadOnly(true);
    m_localPwdEdit->setReadOnly(true);

    auto *escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escapeShortcut->setContext(Qt::WindowShortcut);
    connect(escapeShortcut, &QShortcut::activated, this, [this]()
            {
        if (m_trayIcon && m_trayIcon->isVisible())
        {
            hide();
            LOG_INFO("Main window hidden to system tray by Escape");
        } });

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    WTSRegisterSessionNotification(reinterpret_cast<HWND>(winId()), NOTIFY_FOR_THIS_SESSION);
#endif
}

void MainWindow::bindUiObjects()
{
    m_allowControlLabel = ui->allow_control_label;
    m_localIdLabel = ui->local_id_label;
    m_localPwdLabel = ui->local_pwd_label;
    m_remoteControlLabel = ui->remote_control_label;
    m_remoteIdLabel = ui->remote_id_label;
    m_remotePwdLabel = ui->remote_pwd_label;
    m_wsConnectStatus = ui->ws_connect_status;
    m_versionLabel = new QLabel(tr("Version: %1").arg(QCoreApplication::applicationVersion()), m_content);
    m_versionLabel->setObjectName(QStringLiteral("mainVersionLabel"));
    m_versionLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_versionLabel->setStyleSheet(QStringLiteral("color: rgba(131, 193, 224, 150);"));
    m_localIdEdit = ui->local_id;
    m_localPwdEdit = ui->local_pwd;
    m_remoteIdEdit = ui->remote_id;
    m_remotePwdEdit = ui->remote_pwd;
    m_localIdBorder = ui->local_id_border;
    m_localPwdBorder = ui->local_pwd_border;
    m_remoteIdBorder = ui->remote_id_border;
    m_remotePwdBorder = ui->remote_pwd_border;
    m_remoteDesktopRadio = ui->remote_desktop;
    m_remoteFileRadio = ui->remote_file;
    m_remoteTerminalRadio = ui->remote_terminal;
    m_connectButton = ui->btn_conn;
    m_localPwdChangeButton = ui->local_pwd_change;
    m_localShareButton = ui->local_share;
    m_settingsButton = ui->mainSettingsButton;
    m_connectDivider = new QWidget(m_content);
    m_localPwdChangeDivider = new QWidget(m_content);
    m_localShareDivider = new QWidget(m_content);
    for (QWidget *divider : {m_connectDivider, m_localPwdChangeDivider, m_localShareDivider})
    {
        divider->setObjectName(QStringLiteral("inputActionDivider"));
        divider->setStyleSheet(QStringLiteral("background-color: #565656; border: 0;"));
    }
    m_settingsButton->setText(QString());
    m_settingsButton->setIcon(makeSettingsIcon());
    m_settingsButton->setIconSize(QSize(20, 20));
    m_settingsButton->setToolTip(tr("设置"));
    m_settingsButton->setAccessibleName(tr("设置"));

    m_localIdEdit->setReadOnly(true);
    m_localPwdEdit->setReadOnly(true);
    m_remoteIdEdit->setClearButtonEnabled(true);
    installPasswordRevealButton(m_remotePwdEdit, true);
    connect(m_remoteIdEdit, &QLineEdit::textChanged, this, [this](const QString &text)
            { tryFillRemoteFieldsFromShareText(text); });
    connect(m_remoteIdEdit, &QLineEdit::editingFinished, this, [this]()
            {
                const QString text = m_remoteIdEdit->text().trimmed();
                if (m_remoteIdEdit->text() != text)
                    m_remoteIdEdit->setText(text);
            });
    connect(m_remotePwdEdit, &QLineEdit::editingFinished, this, [this]()
            {
                const QString text = m_remotePwdEdit->text().trimmed();
                if (m_remotePwdEdit->text() != text)
                    m_remotePwdEdit->setText(text);
            });

    connect(m_settingsButton, &QPushButton::clicked, this, &MainWindow::openSettingsFromTray);
    m_remoteDesktopRadio->setChecked(true);

    for (QWidget *border : {m_localPwdBorder, m_localIdBorder, m_remotePwdBorder, m_remoteIdBorder})
        border->lower();
}

void MainWindow::applyMainScale()
{
    constexpr int kBaseWindowWidth = 800;
    constexpr int kBaseWindowHeight = 638;
    constexpr int kBaseContentWidth = 800;
    constexpr int kBaseContentHeight = 600;

    m_uiScale = UiAdaptive::proportionalScale(this, QSize(kBaseWindowWidth, kBaseWindowHeight), 0.72, 1.0, 0.45);

    if (m_titleBar)
        m_titleBar->setUiScale(m_uiScale);
    if (m_content)
        m_content->setMinimumSize(UiAdaptive::scaledSize(QSize(kBaseContentWidth, kBaseContentHeight), 0.45));
    setMinimumSize(UiAdaptive::scaledSize(QSize(kBaseWindowWidth, kBaseWindowHeight), 0.45));
    resize(scaled(kBaseWindowWidth), scaled(kBaseWindowHeight));
    layoutMainContent();
}

void MainWindow::layoutMainContent()
{
    constexpr int kBaseContentWidth = 800;
    constexpr int kBaseContentHeight = 600;

    if (m_content)
    {
        const QSize contentSize = m_content->size();
        if (!contentSize.isEmpty())
        {
            m_uiScale = qMin(contentSize.width() / static_cast<double>(kBaseContentWidth),
                             contentSize.height() / static_cast<double>(kBaseContentHeight));
            m_uiScale = qBound(0.45, m_uiScale, 2.0);
        }
    }

    const int contentW = m_content ? m_content->width() : scaled(kBaseContentWidth);
    const int contentH = m_content ? m_content->height() : scaled(kBaseContentHeight);
    const int groupW = qMax(scaled(300), qMin(contentW - scaled(96), scaled(618)));
    const int groupHeight = scaled(453);
    const int groupX = qMax(scaled(24), (contentW - groupW) / 2);
    const int contentTop = m_content ? m_content->y() : 0;
    const int windowH = height() > 0 ? height() : contentH + contentTop;
    const int groupTop = qMax(scaled(18), (windowH - groupHeight) / 2 - contentTop);

    QFont buttonFont = scaledFont(9);
    for (auto *button : {m_connectButton, m_localPwdChangeButton, m_localShareButton})
    {
        button->setFont(buttonFont);
        button->setStyleSheet(QStringLiteral(
                                  "QPushButton {"
                                  "    font-size: %1pt;"
                                  "    background-color: rgba(120, 48, 65, 92);"
                                  "    border: 0;"
                                  "    border-top-right-radius: %2px;"
                                  "    border-bottom-right-radius: %2px;"
                                  "    border-top-left-radius: 0px;"
                                  "    border-bottom-left-radius: 0px;"
                                  "    color: rgb(131,193,224);"
                                  "    padding: 0px 7px;"
                                  "}"
                                  "QPushButton:hover {"
                                  "    background-color: rgba(120, 48, 65, 150);"
                                  "    color: #d8f3ff;"
                                  "}"
                                  "QPushButton:pressed {"
                                  "    background-color: rgba(93, 38, 52, 170);"
                                  "    color: #ffffff;"
                                  "}")
                                  .arg(buttonFont.pointSizeF(), 0, 'f', 1)
                                  .arg(scaled(8)));
    }

    auto buttonWidth = [this](QPushButton *button, int baseWidth)
    {
        QFontMetrics fm(button->font());
        return qMax(scaled(baseWidth), UiAdaptive::textWidth(fm, button->text()) + scaled(16));
    };

    const int shareButtonW = buttonWidth(m_localShareButton, 54);
    const int updateButtonW = buttonWidth(m_localPwdChangeButton, 54);
    const int connectButtonW = buttonWidth(m_connectButton, 62);
    const int buttonColumnW = qMax(connectButtonW, qMax(shareButtonW, updateButtonW));
    const int fieldX = groupX;
    const int fieldW = groupW;
    const int inputLeftInset = scaled(8);
    const int inputRightInset = scaled(1);
    const int actionGap = scaled(10);
    const int actionH = scaled(43);
    const int actionYInset = scaled(1);
    const int dividerW = qMax(1, scaled(1));
    const int dividerH = qMax(1, actionH * 2 / 3);
    const int dividerYInset = actionYInset + (actionH - dividerH) / 2;
    const int inputW = fieldW - scaled(16);
    const int baseLabelX = fieldX;
    const int sectionLabelX = fieldX;
    const int actionX = fieldX + fieldW - inputRightInset - buttonColumnW;
    const int actionInputW = qMax(scaled(180), actionX - actionGap - (fieldX + inputLeftInset));

    m_allowControlLabel->setFont(scaledFont(13));
    QFontMetrics fmAllow(m_allowControlLabel->font());
    int allowW = qMax(scaled(131), UiAdaptive::textWidth(fmAllow, m_allowControlLabel->text()) + scaled(20));
    m_allowControlLabel->setGeometry(sectionLabelX, groupTop, allowW, scaled(31));

    m_remoteControlLabel->setFont(scaledFont(13));
    QFontMetrics fmRemoteControl(m_remoteControlLabel->font());
    int remoteControlW = qMax(scaled(131), UiAdaptive::textWidth(fmRemoteControl, m_remoteControlLabel->text()) + scaled(20));
    m_remoteControlLabel->setGeometry(sectionLabelX, groupTop + scaled(252), remoteControlW, scaled(31));

    QFont smallFont = scaledFont(11);
    QFontMetrics fmSmall(smallFont);

    m_localIdLabel->setFont(smallFont);
    int localIdW = qMax(scaled(76), UiAdaptive::textWidth(fmSmall, m_localIdLabel->text()) + scaled(10));
    m_localIdLabel->setGeometry(baseLabelX, groupTop + scaled(44), localIdW, scaled(18));

    m_localPwdLabel->setFont(smallFont);
    int localPwdW = qMax(scaled(76), UiAdaptive::textWidth(fmSmall, m_localPwdLabel->text()) + scaled(10));
    m_localPwdLabel->setGeometry(baseLabelX, groupTop + scaled(128), localPwdW, scaled(18));

    m_remoteIdLabel->setFont(smallFont);
    int remoteIdW = qMax(scaled(76), UiAdaptive::textWidth(fmSmall, m_remoteIdLabel->text()) + scaled(10));
    m_remoteIdLabel->setGeometry(baseLabelX, groupTop + scaled(296), remoteIdW, scaled(18));

    m_remotePwdLabel->setFont(smallFont);
    int remotePwdW = qMax(scaled(76), UiAdaptive::textWidth(fmSmall, m_remotePwdLabel->text()) + scaled(10));
    m_remotePwdLabel->setGeometry(baseLabelX, groupTop + scaled(380), remotePwdW, scaled(18));

    m_wsConnectStatus->setGeometry(scaled(20), (m_content ? m_content->height() - scaled(30) : scaled(570)), scaled(281), scaled(18));
    if (m_versionLabel)
    {
        m_versionLabel->setFont(scaledFont(9));
        QFontMetrics fmVersion(m_versionLabel->font());
        const int versionW = qMax(scaled(120), UiAdaptive::textWidth(fmVersion, m_versionLabel->text()) + scaled(12));
        m_versionLabel->setGeometry(contentW - versionW - scaled(20),
                                    (m_content ? m_content->height() - scaled(30) : scaled(570)),
                                    versionW,
                                    scaled(18));
    }

    m_localIdBorder->setGeometry(fieldX, groupTop + scaled(72), fieldW, scaled(45));
    m_localPwdBorder->setGeometry(fieldX, groupTop + scaled(156), fieldW, scaled(45));
    m_remoteIdBorder->setGeometry(fieldX, groupTop + scaled(324), fieldW, scaled(45));
    m_remotePwdBorder->setGeometry(fieldX, groupTop + scaled(408), fieldW, scaled(45));

    m_localIdEdit->setFont(scaledFont(15));
    m_localIdEdit->setGeometry(fieldX + inputLeftInset, groupTop + scaled(81), actionInputW, scaled(30));

    m_localPwdEdit->setFont(scaledFont(15));
    m_localPwdEdit->setGeometry(fieldX + inputLeftInset, groupTop + scaled(165), actionInputW, scaled(30));

    m_remoteIdEdit->setFont(scaledFont(15));
    m_remoteIdEdit->setGeometry(fieldX + inputLeftInset, groupTop + scaled(333), inputW, scaled(30));

    const QFont remotePwdFont = scaledFont(15);
    m_remotePwdEdit->setFont(remotePwdFont);
    m_remotePwdEdit->setGeometry(fieldX + inputLeftInset, groupTop + scaled(417), actionInputW, scaled(30));
    setPasswordRevealFonts(m_remotePwdEdit, remotePwdFont, remotePwdFont);

    const int radioGap = scaled(12);
    int radioY = groupTop + scaled(259);
    int radioX = sectionLabelX + remoteControlW + scaled(24);
    for (auto *radio : {m_remoteDesktopRadio, m_remoteFileRadio, m_remoteTerminalRadio})
    {
        radio->setFont(scaledFont(11));
    }
    auto radioWidth = [](QRadioButton *radio)
    {
        return UiAdaptive::textWidth(radio->fontMetrics(), radio->text()) + 28;
    };
    const int desktopRadioW = qMax(scaled(95), radioWidth(m_remoteDesktopRadio));
    const int fileRadioW = qMax(scaled(95), radioWidth(m_remoteFileRadio));
    const int terminalRadioW = qMax(scaled(95), radioWidth(m_remoteTerminalRadio));
    const int totalRadioW = desktopRadioW + fileRadioW + terminalRadioW + radioGap * 2;
    if (radioX + totalRadioW > groupX + groupW)
    {
        radioX = baseLabelX;
        radioY = groupTop + scaled(284);
    }
    m_remoteDesktopRadio->setGeometry(radioX, radioY, desktopRadioW, scaled(22));
    radioX += desktopRadioW + radioGap;
    m_remoteFileRadio->setGeometry(radioX, radioY, fileRadioW, scaled(22));
    radioX += fileRadioW + radioGap;
    m_remoteTerminalRadio->setGeometry(radioX, radioY, terminalRadioW, scaled(22));

    m_connectButton->setGeometry(actionX, groupTop + scaled(408) + actionYInset, buttonColumnW, actionH);
    m_localPwdChangeButton->setGeometry(actionX, groupTop + scaled(156) + actionYInset, buttonColumnW, actionH);
    m_localShareButton->setGeometry(actionX, groupTop + scaled(72) + actionYInset, buttonColumnW, actionH);
    m_connectDivider->setGeometry(actionX, groupTop + scaled(408) + dividerYInset, dividerW, dividerH);
    m_localPwdChangeDivider->setGeometry(actionX, groupTop + scaled(156) + dividerYInset, dividerW, dividerH);
    m_localShareDivider->setGeometry(actionX, groupTop + scaled(72) + dividerYInset, dividerW, dividerH);
    m_connectDivider->raise();
    m_localPwdChangeDivider->raise();
    m_localShareDivider->raise();
    m_connectButton->raise();
    m_localPwdChangeButton->raise();
    m_localShareButton->raise();
    m_settingsButton->setGeometry(contentW - scaled(44), scaled(12), scaled(36), scaled(36));
    m_settingsButton->setIconSize(QSize(scaled(20), scaled(20)));
}

int MainWindow::scaled(int value) const
{
    return qMax(1, static_cast<int>(qRound(value * m_uiScale)));
}

QRect MainWindow::scaledRect(int x, int y, int width, int height) const
{
    return QRect(scaled(x), scaled(y), scaled(width), scaled(height));
}

QFont MainWindow::scaledFont(double pointSize, bool bold) const
{
    QFont font;
    font.setPointSizeF(qMax(4.5, pointSize * m_uiScale));
    font.setBold(bold);
    return font;
}

bool MainWindow::tryFillRemoteFieldsFromShareText(const QString &text)
{
    if (m_remoteShareParsing)
        return false;

    const QString normalized = text.trimmed();
    if (normalized.size() < 8)
        return false;

    QString remoteId = firstCapture(normalized, {QStringLiteral("(?:识别码|远端识别码|设备码|ID|id)\\s*[:：]?\\s*([A-Za-z0-9][A-Za-z0-9_\\-{}]{5,})"),
                                                 QStringLiteral("(?:sessionId|remoteId|deviceId)\\s*=\\s*([A-Za-z0-9][A-Za-z0-9_\\-{}]{5,})")});
    QString remotePwd = firstCapture(normalized, {QStringLiteral("(?:验证码|远端验证码|密码|Password|password|pwd|code)\\s*[:：]?\\s*([A-Za-z0-9][A-Za-z0-9_\\-{}]{3,})"),
                                                  QStringLiteral("(?:remotePwd|pwd|password)\\s*=\\s*([A-Za-z0-9][A-Za-z0-9_\\-{}]{3,})")});

    if (remoteId.isEmpty() || remotePwd.isEmpty())
        return false;

    m_remoteShareParsing = true;
    const QSignalBlocker idBlocker(m_remoteIdEdit);
    const QSignalBlocker pwdBlocker(m_remotePwdEdit);
    m_remoteIdEdit->setText(remoteId.remove(QChar('{')).remove(QChar('}')).trimmed());
    m_remotePwdEdit->setText(remotePwd.remove(QChar('{')).remove(QChar('}')).trimmed());
    m_remotePwdEdit->setFocus();
    m_remoteShareParsing = false;
    LOG_INFO("Parsed remote share text into remote id and password");
    return true;
}
