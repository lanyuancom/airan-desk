/* Split from control_window_view.cpp to keep ControlWindow UI responsibilities modular. */

#include "control_window.h"
#include "ui/adaptive_ui.h"

#include <QApplication>
#include <QLabel>
#include <QScreen>
#include <QStyle>

void ControlWindow::updateImg(const QImage &img)
{
    if (img.isNull() || img.width() <= 0 || img.height() <= 0)
    {
        LOG_WARN("Received invalid image: null={}, size={}x{}",
                 img.isNull(), img.width(), img.height());
        return;
    }

    isReceivedImg = true;
    if (m_remoteResolution != img.size())
    {
        m_remoteResolution = img.size();
        refreshStatsLabel();
        updateAndroidSidePanelWidth();
        updateToolbarPosition();
        if (windowSizeAdjusted)
            windowSizeAdjusted = false;
    }

    if (!m_fpsTimer.isValid())
    {
        m_fpsTimer.start();
    }
    ++m_fpsFrameCount;
    const qint64 elapsedMs = m_fpsTimer.elapsed();
    if (elapsedMs >= 1000)
    {
        m_currentFps = (m_fpsFrameCount * 1000.0) / elapsedMs;
        m_fpsFrameCount = 0;
        m_fpsTimer.restart();
        refreshStatsLabel();
    }

    if (!windowSizeAdjusted)
    {
        adjustWindowSizeToVideo(img.size());
    }
    m_windowSize = img.size();

    QPixmap pixmap = QPixmap::fromImage(img, Qt::ColorOnly);
    if (pixmap.isNull())
    {
        LOG_ERROR("Failed to convert QImage to QPixmap, image size: {}x{}, format: {}",
                  img.width(), img.height(), static_cast<int>(img.format()));
        return;
    }

    m_sourcePixmap = pixmap;
    if (!m_fitToWindow)
    {
        label.resize(m_sourcePixmap.size());
    }
    updateScaledPixmap();
}

void ControlWindow::updateVideoStats(double kbps, const QSize &resolution)
{
    m_currentKbps = kbps;
    if (resolution.isValid() && m_remoteResolution != resolution)
    {
        m_remoteResolution = resolution;
        updateAndroidSidePanelWidth();
        updateToolbarPosition();
    }
    refreshStatsLabel();
}

void ControlWindow::onRemoteDesktopStateChanged(bool locked, const QString &message)
{
    m_remoteDesktopLocked = locked;
    if (!locked)
    {
        if (!isReceivedImg)
        {
            label.setText(tr("正在连接..."));
        }
        return;
    }

    label.clear();
    label.setText(message.isEmpty()
                      ? tr("远端 Windows 已锁屏。当前应用模式无法显示密码输入界面，请在被控端本机解锁。")
                      : message);
    label.setAlignment(Qt::AlignCenter);
    label.resize(scrollArea.viewport()->size());
    LOG_INFO("Remote desktop is locked; showing lock notice");
}

void ControlWindow::adjustWindowSizeToVideo(const QSize &videoSize)
{
    LOG_INFO("Adjusting window size to match video: {}x{}", videoSize.width(), videoSize.height());

    QScreen *screen = QApplication::primaryScreen();
    QRect screenGeometry = UiAdaptive::availableGeometry(this);

    LOG_INFO("Screen available geometry: {}x{}", screenGeometry.width(), screenGeometry.height());
    const int titleBarHeight = menuWidget() ? menuWidget()->sizeHint().height()
                                            : style()->pixelMetric(QStyle::PM_TitleBarHeight);
    const int sidePanelWidth = (m_androidNavHost && m_androidNavHost->isVisible()) ? m_androidNavHost->width() : 0;
    const int maxContentWidth = qMax(320, screenGeometry.width() - sidePanelWidth);
    const int maxContentHeight = qMax(240, screenGeometry.height() - titleBarHeight);

    const QSize videoDisplaySize = videoSize.scaled(maxContentWidth, maxContentHeight, Qt::KeepAspectRatio);
    const QSize initialWindowSize(videoDisplaySize.width() + sidePanelWidth,
                                  videoDisplaySize.height() + titleBarHeight);

    bool needMaximize = initialWindowSize.height() > screenGeometry.height() ||
                        initialWindowSize.width() > screenGeometry.width();
    if (needMaximize)
    {
        this->showMaximized();
    }
    else
    {
        resize(initialWindowSize);
    }

    scrollArea.updateGeometry();
    this->updateGeometry();

    if (!needMaximize && screen)
    {
        QRect windowGeometry = this->geometry();
        windowGeometry.moveCenter(screen->geometry().center());

        if (windowGeometry.left() < screenGeometry.left())
            windowGeometry.moveLeft(screenGeometry.left());
        if (windowGeometry.top() < screenGeometry.top())
            windowGeometry.moveTop(screenGeometry.top());
        if (windowGeometry.right() > screenGeometry.right())
            windowGeometry.moveRight(screenGeometry.right());
        if (windowGeometry.bottom() > screenGeometry.bottom())
            windowGeometry.moveBottom(screenGeometry.bottom());

        this->setGeometry(windowGeometry);

        LOG_INFO("Window positioned at: ({}, {}), size: {}x{}",
                 windowGeometry.x(), windowGeometry.y(),
                 windowGeometry.width(), windowGeometry.height());
    }

    windowSizeAdjusted = true;
    updateScaledPixmap();
    updateToolbarPosition();
}

void ControlWindow::updateScaledPixmap()
{
    if (m_sourcePixmap.isNull())
        return;

    if (!m_fitToWindow)
    {
        const QSize sourceSize = m_sourcePixmap.size();
        if (sourceSize.isEmpty())
            return;

        if (label.size() != sourceSize)
        {
            label.resize(sourceSize);
        }

        m_videoDisplayRect = QRect(QPoint(0, 0), sourceSize);
        label.setPixmap(m_sourcePixmap);
        label.update();
        return;
    }

    const QSize targetSize = scrollArea.viewport()->size();
    if (targetSize.isEmpty())
        return;

    label.resize(targetSize);

    const QSize scaledSize = m_sourcePixmap.size().scaled(targetSize, Qt::KeepAspectRatio);
    if (scaledSize.isEmpty())
        return;

    m_videoDisplayRect = QRect(QPoint((targetSize.width() - scaledSize.width()) / 2,
                                      (targetSize.height() - scaledSize.height()) / 2),
                               scaledSize);
    label.setPixmap(m_sourcePixmap.scaled(scaledSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    label.update();
}

