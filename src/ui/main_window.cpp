#include "main_window.h"
#include "common/constant.h"
#include "ui_main_window.h"
#include <QCoreApplication>
#include <QThread>

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#include <windows.h>
#include <wtsapi32.h>
#endif

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent),
      windowTitle(tr("AiRan")),
      textToCopy(tr("欢迎使用%1远程工具，您的识别码：%2\n验证码: %3")),
      isCaptureing(false)
{
    if (ConfigUtil->showUI)
    {
        initUI();
        initTray();
    }
    initCli();
}

MainWindow::~MainWindow()
{
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    if (ui)
        WTSUnRegisterSessionNotification(reinterpret_cast<HWND>(winId()));
#endif
    disconnect();
    cleanupWebRtcCliSessions();
    if (m_ws_thread.isRunning())
    {
        QMetaObject::invokeMethod(&m_ws, "shutdown", Qt::BlockingQueuedConnection);
    }
    STOP_OBJ_THREAD(m_ws_thread);
    delete ui;
}
