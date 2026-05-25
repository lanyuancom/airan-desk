/* Split from control_window_view.cpp to keep ControlWindow UI responsibilities modular. */

#include "control_window.h"

void ControlWindow::setAndroidNavigationVisible(bool visible)
{
    m_androidNavigationVisible = visible;
    if (m_androidNavHost)
    {
        updateAndroidSidePanelWidth();
        if (visible)
            constrainAndroidNavigationPanel();
    }
    if (m_centralHost)
        m_centralHost->updateGeometry();
    updateToolbarPosition();
    if (isReceivedImg && !m_sourcePixmap.isNull())
    {
        adjustWindowSizeToVideo(m_sourcePixmap.size());
    }
    else
    {
        updateScaledPixmap();
    }
}

void ControlWindow::constrainAndroidNavigationPanel()
{
    if (!m_androidNavigationVisible || !m_androidNavPanel || !m_androidNavHost || !m_androidNavHost->isVisible())
        return;

    m_androidNavPanel->adjustSize();
    const int margin = 8;
    const int maxX = qMax(0, m_androidNavHost->width() - m_androidNavPanel->width() - margin);
    const int maxY = qMax(0, m_androidNavHost->height() - m_androidNavPanel->height() - margin);

    QPoint target = m_androidNavPanel->pos();
    if (target.isNull())
    {
        const int toolbarBottom = (shouldPlaceToolbarInSidePanel() && m_floatingToolbar)
                                      ? (m_floatingToolbar->y() + m_floatingToolbar->height() + margin)
                                      : margin;
        target = QPoint(maxX, qMin(toolbarBottom, maxY));
    }

    target.setX(qBound(margin, target.x(), maxX));
    target.setY(qBound(margin, target.y(), maxY));
    if (shouldPlaceToolbarInSidePanel() && m_floatingToolbar)
    {
        const QRect toolbarRect(m_floatingToolbar->pos(), m_floatingToolbar->size());
        const QRect navRect(target, m_androidNavPanel->size());
        if (toolbarRect.intersects(navRect))
            target.setY(qBound(margin, m_floatingToolbar->y() + m_floatingToolbar->height() + margin, maxY));
    }
    m_androidNavPanel->move(target);
}

