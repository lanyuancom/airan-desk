#include "main_window.h"
#include "settings_window.h"
#include "common/constant.h"
#include <QApplication>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QResizeEvent>
#include <QSignalBlocker>

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#include <windows.h>
#include <wtsapi32.h>
#endif

void MainWindow::initTray()
{
    if (!ConfigUtil->showUI)
        return;

    if (!QSystemTrayIcon::isSystemTrayAvailable())
    {
        LOG_WARN("System tray is not available");
        return;
    }

    m_trayMenu = new QMenu(this);
    m_trayOpenAction = m_trayMenu->addAction(tr("打开窗口"), this, &MainWindow::showWindowFromTray);
    m_traySettingsAction = m_trayMenu->addAction(tr("设置"), this, &MainWindow::openSettingsFromTray);
    m_trayDiagnosticsAction = m_trayMenu->addAction(tr("运行时诊断"), this, &MainWindow::showRuntimeDiagnostics);
    m_trayMenu->addSeparator();
    m_trayQuitAction = m_trayMenu->addAction(tr("退出"), this, &MainWindow::quitFromTray);

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setToolTip(windowTitle);
    m_trayIcon->setContextMenu(m_trayMenu);
    m_trayIcon->setIcon(qApp->windowIcon());
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason)
            {
        if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger)
            showWindowFromTray(); });
    m_trayIcon->show();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_trayIcon && m_trayIcon->isVisible())
    {
        hide();
        event->ignore();
        LOG_INFO("Main window hidden to system tray");
        return;
    }

    QWidget::closeEvent(event);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && m_trayIcon && m_trayIcon->isVisible())
    {
        hide();
        event->accept();
        LOG_INFO("Main window hidden to system tray by Escape");
        return;
    }

    QWidget::keyPressEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    constexpr double kMainAspect = 800.0 / 638.0;
    if (event && !event->size().isEmpty())
    {
        const int expectedHeight = qMax(minimumHeight(), static_cast<int>(qRound(width() / kMainAspect)));
        if (qAbs(expectedHeight - height()) > 1)
        {
            QSignalBlocker blocker(this);
            resize(width(), expectedHeight);
            return;
        }
    }
    if (m_content)
        layoutMainContent();
}

void MainWindow::showWindowFromTray()
{
    if (!ConfigUtil->showUI)
        return;

    showNormal();
    raise();
    activateWindow();
}

void MainWindow::openSettingsFromTray()
{
    if (!ConfigUtil->showUI)
        return;

    if (m_settingsWindow)
    {
        m_settingsWindow->show();
        m_settingsWindow->raise();
        m_settingsWindow->activateWindow();
        return;
    }

    m_settingsWindow = new SettingsWindow(this);
    m_settingsWindow->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_settingsWindow, &QObject::destroyed, this, [this]() {
        m_settingsWindow = nullptr;
    });
    m_settingsWindow->show();
    m_settingsWindow->raise();
    m_settingsWindow->activateWindow();
}

void MainWindow::showRuntimeDiagnostics()
{
    QString text;
    text += tr("App: %1").arg(windowTitle) + QLatin1Char('\n');
    text += tr("WebSocket url: %1").arg(ConfigUtil->wsUrl) + QLatin1Char('\n');
    text += tr("WebSocket thread running: %1").arg(m_ws_thread.isRunning()) + QLatin1Char('\n');
    text += tr("RTC sessions: %1").arg(m_rtcCliSessions.size()) + QLatin1Char('\n');
    text += tr("Desktop locked: %1").arg(m_desktopLocked) + QLatin1Char('\n');
    text += tr("Default stream: quality=%1 bitrate=%2 fps=%3 network=%4 capture=%5")
                .arg(ConfigUtil->remote_quality,
                     ConfigUtil->remote_bitrate_profile,
                     QString::number(ConfigUtil->fps),
                     ConfigUtil->remote_network_path,
                     ConfigUtil->remote_capture_backend) +
            QLatin1Char('\n');
    text += tr("Default remote size: %1x%2").arg(ConfigUtil->remote_width).arg(ConfigUtil->remote_height) + QLatin1Char('\n');
    text += tr("Local id: %1").arg(ConfigUtil->local_id) + QLatin1Char('\n');

    auto *box = new QMessageBox(this);
    box->setWindowTitle(tr("运行时诊断"));
    box->setTextFormat(Qt::PlainText);
    box->setText(text);
    box->setStandardButtons(QMessageBox::Ok);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->show();
}

void MainWindow::quitFromTray()
{
    if (m_trayIcon)
        m_trayIcon->hide();
    qApp->quit();
}

void MainWindow::updateDesktopStateForSessions(bool locked, const QString &message)
{
    m_desktopLocked = locked;
    for (auto it = m_rtcCliSessions.begin(); it != m_rtcCliSessions.end(); ++it)
    {
        WebRtcCli *cli = it.key();
        if (!cli)
            continue;

        QMetaObject::invokeMethod(cli, "setDesktopLocked", Qt::QueuedConnection, Q_ARG(bool, locked));
    }

    LOG_INFO("Windows session desktop state changed: locked={}, message={}", locked, message);
}

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, long *result)
{
    Q_UNUSED(eventType);
    Q_UNUSED(result);

    MSG *msg = static_cast<MSG *>(message);
    if (!msg || msg->message != WM_WTSSESSION_CHANGE)
        return false;

    if (msg->wParam == WTS_SESSION_LOCK)
    {
        updateDesktopStateForSessions(true, QStringLiteral("locked"));
    }
    else if (msg->wParam == WTS_SESSION_UNLOCK)
    {
        updateDesktopStateForSessions(false, QStringLiteral("unlocked"));
    }
    return false;
}
#endif
