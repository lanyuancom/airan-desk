/* Split from control_window_view.cpp to keep ControlWindow UI responsibilities modular. */

#include "control_window.h"
#include "file_transfer_window.h"
#include "ui/control_window_view_helpers.h"
#include "util/config_util.h"
#include "util/json_util.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QLabel>
#include <QMenu>
#include <QMetaObject>
#include <QPushButton>
#include <QTimer>

void ControlWindow::onScreenshotClicked()
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    QPixmap screenshot = label.pixmap(Qt::ReturnByValue);
#else
    const QPixmap *labelPixmap = label.pixmap();
    QPixmap screenshot = labelPixmap ? *labelPixmap : QPixmap();
#endif
    if (screenshot.isNull())
    {
        LOG_WARN("No image available for screenshot");
        return;
    }

    /* 复制到系统剪切板 */
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setPixmap(screenshot);

    LOG_INFO("Screenshot copied to clipboard, size: {}x{}",
             screenshot.width(), screenshot.height());

    /* 简单的视觉反馈 */
    m_screenshotBtn->setText(tr("已复制"));
    QTimer::singleShot(1000, this, &ControlWindow::restoreScreenshotButtonText);
}

void ControlWindow::onRemoteEncoderChanged(const QString &encoderName, const QString &encoderType, bool zeroCopy)
{
    m_remoteEncoderName = encoderName;
    m_remoteEncoderType = encoderType;
    m_remoteEncoderZeroCopy = zeroCopy;
    refreshStatsLabel();
}

void ControlWindow::restoreScreenshotButtonText()
{
    m_screenshotBtn->setText(tr("📸 截屏"));
}

void ControlWindow::onFileTransferClicked()
{
    if (!ConfigUtil->showUI)
        return;

    /* 打开独立的文件传输窗口（不设置父窗口） */
    FileTransferWindow *fileWindow = new FileTransferWindow(remote_id, remote_pwd_md5, m_ws, this, &m_rtc_ctl);
    fileWindow->setAttribute(Qt::WA_DeleteOnClose);
    fileWindow->setWindowTitle(tr("文件传输 - %1").arg(remote_id));
    fileWindow->show();
    fileWindow->raise();
    fileWindow->activateWindow();

    LOG_INFO("Shared-session file transfer window opened");
}

void ControlWindow::onAudioCaptureClicked()
{
    QMenu menu(this);
    QAction *offAction = menu.addAction(tr("关闭"));
    offAction->setData(QStringLiteral("off"));
    offAction->setCheckable(true);
    offAction->setChecked(m_audioMode == QStringLiteral("off"));
    QAction *listenAction = menu.addAction(tr("侦听"));
    listenAction->setData(QStringLiteral("listen"));
    listenAction->setCheckable(true);
    listenAction->setChecked(m_audioMode == QStringLiteral("listen"));
    QAction *callAction = menu.addAction(tr("通话"));
    callAction->setData(QStringLiteral("call"));
    callAction->setCheckable(true);
    callAction->setChecked(m_audioMode == QStringLiteral("call"));

    QAction *selected = menu.exec(m_audioCaptureBtn ? m_audioCaptureBtn->mapToGlobal(QPoint(0, m_audioCaptureBtn->height())) : QCursor::pos());
    if (!selected)
        return;

    m_audioMode = selected->data().toString();
    m_audioCaptureEnabled = m_audioMode != QStringLiteral("off");
    if (m_audioCaptureBtn)
    {
        QString text = tr("音频关闭");
        if (m_audioMode == QStringLiteral("listen"))
            text = tr("侦听");
        else if (m_audioMode == QStringLiteral("call"))
            text = tr("通话");
        m_audioCaptureBtn->setText(text);
        m_audioCaptureBtn->setToolTip(tr("当前音频模式：%1").arg(text));
        if (shouldPlaceToolbarInSidePanel())
            applyToolbarLayoutMode(true);
        else
            fitControlButtonWidthToText(m_audioCaptureBtn);
    }

    QMetaObject::invokeMethod(&m_rtc_ctl, "setAudioMode", Qt::QueuedConnection,
                              Q_ARG(QString, m_audioMode));

    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_AUDIO_CAPTURE)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_ENABLED, m_audioCaptureEnabled)
                          .add(Constant::KEY_AUDIO_MODE, m_audioMode)
                          .build();

    rtc::message_variant msgStr(JsonUtil::toCompactBytes(obj).toStdString());
    emit sendMsg2InputChannel(msgStr);
    LOG_INFO("Remote audio mode changed: {}", m_audioMode);
}
