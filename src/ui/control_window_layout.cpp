/* Split from control_window_view.cpp to keep ControlWindow UI responsibilities modular. */

#include "control_window.h"
#include "ui/control_window_view_helpers.h"

#include <QBoxLayout>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QSizePolicy>
#include <QToolButton>

namespace
{
    int textWidth(const QFontMetrics &metrics, const QString &text)
    {
        return controlWindowTextWidth(metrics, text);
    }

    void fitButtonWidthToText(QPushButton *button)
    {
        fitControlButtonWidthToText(button);
    }
}

bool ControlWindow::isRemotePortrait() const
{
    return m_remoteResolution.isValid() &&
           m_remoteResolution.height() > m_remoteResolution.width();
}

bool ControlWindow::shouldPlaceToolbarInSidePanel() const
{
    return m_androidNavHost &&
           isRemotePortrait();
}

void ControlWindow::applyToolbarLayoutMode(bool sidePanelMode)
{
    if (!m_floatingToolbar || !m_toolbarButtonLayout || !m_statsLabel)
        return;

    constexpr int sideControlWidth = 124;

    m_toolbarButtonLayout->setDirection(sidePanelMode ? QBoxLayout::TopToBottom
                                                      : QBoxLayout::LeftToRight);
    m_toolbarButtonLayout->setSpacing(sidePanelMode ? 6 : 4);

    if (auto *layout = m_floatingToolbar->layout())
    {
        layout->setContentsMargins(sidePanelMode ? 8 : 24,
                                   sidePanelMode ? 8 : 7,
                                   sidePanelMode ? 8 : 24,
                                   sidePanelMode ? 8 : 8);
        layout->setSpacing(sidePanelMode ? 6 : 5);
    }

    auto applyButtonWidth = [sidePanelMode, sideControlWidth](QPushButton *button)
    {
        if (!button)
            return;
        if (sidePanelMode)
        {
            button->setFixedWidth(sideControlWidth);
            button->setMinimumHeight(30);
        }
        else
        {
            fitButtonWidthToText(button);
            button->setMinimumHeight(0);
        }
    };
    applyButtonWidth(m_screenshotBtn);
    applyButtonWidth(m_remoteOperationBtn);
    applyButtonWidth(m_fileTransferBtn);
    applyButtonWidth(m_audioCaptureBtn);
    applyButtonWidth(m_diagnosticsBtn);
    applyButtonWidth(m_moreBtn);

    if (m_moreBtn)
        m_moreBtn->setVisible(!sidePanelMode);
    if (m_toolbarOptionsPanel)
    {
        m_toolbarOptionsPanel->setVisible(sidePanelMode);
        m_toolbarOptionsPanel->setMinimumWidth(sidePanelMode ? sideControlWidth : 0);
        m_toolbarOptionsPanel->setMaximumWidth(sidePanelMode ? sideControlWidth : 0);
    }
    for (QToolButton *button : m_sideMenuButtons)
    {
        if (!button)
            continue;
        button->setFixedWidth(sideControlWidth);
        button->setMinimumHeight(38);
    }
    if (m_toolbarButtonRow)
    {
        if (sidePanelMode)
        {
            m_toolbarButtonRow->setFixedWidth(sideControlWidth);
        }
        else
        {
            m_toolbarButtonRow->setMinimumWidth(0);
            m_toolbarButtonRow->setMaximumWidth(QWIDGETSIZE_MAX);
            m_toolbarButtonRow->adjustSize();
        }
    }

    m_statsLabel->setWordWrap(sidePanelMode);
    m_statsLabel->setAlignment(sidePanelMode ? (Qt::AlignLeft | Qt::AlignVCenter)
                                             : Qt::AlignCenter);
    if (sidePanelMode)
    {
        m_statsLabel->setMinimumWidth(sideControlWidth);
        m_statsLabel->setMaximumWidth(sideControlWidth);
    }
    else
    {
        QFontMetrics statsFm(m_statsLabel->font());
        int statsTextW = textWidth(statsFm, m_statsLabel->text());
        const int padding = 24;
        const int minAdaptive = qMax(220, statsTextW + padding);
        m_statsLabel->setMinimumWidth(minAdaptive);
        m_statsLabel->setMaximumWidth(QWIDGETSIZE_MAX);
    }
    m_statsLabel->setStyleSheet(QStringLiteral("QLabel { color: rgba(255,255,255,230); background: transparent; border: none; padding: 0px 2px; font-size: %1px; }")
                                    .arg(sidePanelMode ? 12 : 10));

    if (m_toolbarButtonRow)
        m_toolbarButtonRow->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void ControlWindow::updateAndroidSidePanelWidth()
{
    if (!m_androidNavHost)
        return;

    const bool sidePanelMode = isRemotePortrait();
    const bool hostVisible = m_androidNavigationVisible || sidePanelMode;

    m_androidNavHost->setVisible(hostVisible);
    if (m_androidNavPanel)
        m_androidNavPanel->setVisible(m_androidNavigationVisible);

    if (!hostVisible)
    {
        m_androidNavHost->setFixedWidth(0);
        return;
    }

    applyToolbarLayoutMode(sidePanelMode);

    int width = 152;
    if (sidePanelMode && m_floatingToolbar)
    {
        m_floatingToolbar->adjustSize();
        width = qMax(width, m_floatingToolbar->width() + 16);
    }

    m_androidNavHost->setFixedWidth(width);
    m_androidNavHost->updateGeometry();
}

void ControlWindow::updateToolbarPosition()
{
    if (!m_floatingToolbar)
        return;

    const bool sidePanelMode = shouldPlaceToolbarInSidePanel();
    const bool parentChanged = sidePanelMode != m_toolbarInSidePanel;

    applyToolbarLayoutMode(sidePanelMode);
    updateAndroidSidePanelWidth();

    QWidget *targetParent = sidePanelMode ? m_androidNavHost : static_cast<QWidget *>(this);
    if (targetParent && m_floatingToolbar->parentWidget() != targetParent)
    {
        const QPoint globalTopLeft = m_floatingToolbar->mapToGlobal(QPoint(0, 0));
        m_floatingToolbar->setParent(targetParent);
        m_floatingToolbar->move(targetParent->mapFromGlobal(globalTopLeft));
        m_floatingToolbar->show();
    }

    m_toolbarInSidePanel = sidePanelMode;
    m_floatingToolbar->adjustSize();

    QWidget *toolbarParent = m_floatingToolbar->parentWidget();
    if (!toolbarParent)
        toolbarParent = this;

    QPoint target = m_floatingToolbar->pos();
    if (parentChanged || !m_toolbarUserMoved)
    {
        if (sidePanelMode)
        {
            const int margin = 8;
            const int gap = 12;
            int groupHeight = m_floatingToolbar->height();
            if (m_androidNavigationVisible && m_androidNavPanel)
            {
                m_androidNavPanel->adjustSize();
                groupHeight += gap + m_androidNavPanel->height();
            }
            target = QPoint(qMax(0, (toolbarParent->width() - m_floatingToolbar->width()) / 2),
                            qMax(margin, (toolbarParent->height() - groupHeight) / 2));
        }
        else
        {
            target = QPoint(qMax(0, (toolbarParent->width() - m_floatingToolbar->width()) / 2), 10);
        }
    }

    const int maxX = qMax(0, toolbarParent->width() - m_floatingToolbar->width());
    const int maxY = qMax(0, toolbarParent->height() - m_floatingToolbar->height());
    target.setX(qBound(0, target.x(), maxX));
    target.setY(qBound(0, target.y(), maxY));
    m_floatingToolbar->move(target);
    m_floatingToolbar->raise();
    if (sidePanelMode && m_androidNavigationVisible && m_androidNavPanel && (parentChanged || !m_toolbarUserMoved))
    {
        const int gap = 12;
        const int maxNavX = qMax(0, toolbarParent->width() - m_androidNavPanel->width());
        const int maxNavY = qMax(0, toolbarParent->height() - m_androidNavPanel->height());
        QPoint navTarget(qBound(0, (toolbarParent->width() - m_androidNavPanel->width()) / 2, maxNavX),
                         qBound(0, m_floatingToolbar->y() + m_floatingToolbar->height() + gap, maxNavY));
        m_androidNavPanel->move(navTarget);
    }
    if (sidePanelMode)
        constrainAndroidNavigationPanel();
}
