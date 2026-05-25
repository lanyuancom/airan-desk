#include "app_title_bar.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSize>

namespace
{
    enum class TitleBarGlyph
    {
        Minimize,
        Maximize,
        Restore,
        Close
    };

    QIcon makeTitleBarIcon(TitleBarGlyph glyph)
    {
        QPixmap pixmap(16, 16);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(QColor(131, 193, 224), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        switch (glyph)
        {
        case TitleBarGlyph::Minimize:
            painter.drawLine(QPointF(4, 11), QPointF(12, 11));
            break;
        case TitleBarGlyph::Maximize:
            painter.drawRect(QRectF(4.5, 4.5, 7, 7));
            break;
        case TitleBarGlyph::Restore:
            painter.drawRect(QRectF(5.5, 6.5, 6, 6));
            painter.drawLine(QPointF(7, 4.5), QPointF(12.5, 4.5));
            painter.drawLine(QPointF(12.5, 4.5), QPointF(12.5, 10));
            break;
        case TitleBarGlyph::Close:
            painter.drawLine(QPointF(5, 5), QPointF(11, 11));
            painter.drawLine(QPointF(11, 5), QPointF(5, 11));
            break;
        }

        return QIcon(pixmap);
    }
}

AppTitleBar::AppTitleBar(QWidget *targetWindow, bool showMinimize, bool showMaximize, QWidget *parent)
    : QWidget(parent ? parent : targetWindow),
      m_targetWindow(targetWindow)
{
    setObjectName(QStringLiteral("appTitleBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(38);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(14, 0, 8, 0);
    m_layout->setSpacing(6);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setObjectName(QStringLiteral("appTitleBarIcon"));
    m_iconLabel->setAlignment(Qt::AlignCenter);
    m_iconLabel->setFixedSize(QSize(20, 20));
    const QIcon appIcon = (m_targetWindow && !m_targetWindow->windowIcon().isNull())
                              ? m_targetWindow->windowIcon()
                              : qApp->windowIcon();
    m_iconLabel->setPixmap(appIcon.pixmap(QSize(18, 18)));
    m_layout->addWidget(m_iconLabel);

    m_titleLabel = new QLabel(m_targetWindow ? m_targetWindow->windowTitle() : QString(), this);
    m_titleLabel->setObjectName(QStringLiteral("appTitleBarTitle"));
    m_titleLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    m_layout->addWidget(m_titleLabel, 1);

    if (showMinimize)
    {
        m_minimizeButton = createButton(makeTitleBarIcon(TitleBarGlyph::Minimize), tr("Minimize"));
        connect(m_minimizeButton, &QPushButton::clicked, m_targetWindow, &QWidget::showMinimized);
        m_layout->addWidget(m_minimizeButton);
    }

    if (showMaximize)
    {
        m_maximizeButton = createButton(makeTitleBarIcon(TitleBarGlyph::Maximize), tr("Maximize"));
        connect(m_maximizeButton, &QPushButton::clicked, this, &AppTitleBar::toggleMaximize);
        m_layout->addWidget(m_maximizeButton);
    }

    m_closeButton = createButton(makeTitleBarIcon(TitleBarGlyph::Close), tr("Close"));
    m_closeButton->setObjectName(QStringLiteral("appTitleBarClose"));
    connect(m_closeButton, &QPushButton::clicked, m_targetWindow, &QWidget::close);
    m_layout->addWidget(m_closeButton);

    applyStyle();

    if (m_targetWindow)
    {
        connect(m_targetWindow, &QWidget::windowTitleChanged, this, &AppTitleBar::setTitle);
        qApp->installEventFilter(this);
    }
    updateMaximizeButton();
}

AppTitleBar::~AppTitleBar()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

void AppTitleBar::setTitle(const QString &title)
{
    if (m_titleLabel)
        m_titleLabel->setText(title);
}

void AppTitleBar::setUiScale(double scale)
{
    m_scale = scale > 0.0 ? scale : 1.0;
    if (m_layout)
    {
        m_layout->setContentsMargins(scaled(14), 0, scaled(8), 0);
        m_layout->setSpacing(scaled(6));
    }
    if (m_titleLabel)
    {
        QFont font = m_titleLabel->font();
        font.setPointSizeF(13.0 * m_scale);
        font.setBold(true);
        m_titleLabel->setFont(font);
    }
    if (m_iconLabel)
    {
        const QSize iconBox(scaled(20), scaled(20));
        const QSize pixmapSize(scaled(18), scaled(18));
        m_iconLabel->setFixedSize(iconBox);
        const QIcon appIcon = (m_targetWindow && !m_targetWindow->windowIcon().isNull())
                                  ? m_targetWindow->windowIcon()
                                  : qApp->windowIcon();
        m_iconLabel->setPixmap(appIcon.pixmap(pixmapSize));
    }
    if (m_minimizeButton)
    {
        m_minimizeButton->setFixedSize(scaled(36), scaled(28));
        m_minimizeButton->setIconSize(QSize(scaled(12), scaled(12)));
    }
    if (m_maximizeButton)
    {
        m_maximizeButton->setFixedSize(scaled(36), scaled(28));
        m_maximizeButton->setIconSize(QSize(scaled(12), scaled(12)));
    }
    if (m_closeButton)
    {
        m_closeButton->setFixedSize(scaled(36), scaled(28));
        m_closeButton->setIconSize(QSize(scaled(12), scaled(12)));
    }
    setFixedHeight(scaled(38));
    applyStyle();
}

void AppTitleBar::setResizeAspectRatio(const QSize &baseSize)
{
    m_resizeAspectBase = baseSize;
}

QPushButton *AppTitleBar::createButton(const QIcon &icon, const QString &tooltip)
{
    auto *button = new QPushButton(this);
    button->setIcon(icon);
    button->setIconSize(QSize(12, 12));
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(tooltip);
    button->setCursor(Qt::ArrowCursor);
    return button;
}

int AppTitleBar::scaled(int value) const
{
    return qMax(1, static_cast<int>(qRound(value * m_scale)));
}

void AppTitleBar::applyStyle()
{
    setStyleSheet(QStringLiteral(
        "#appTitleBar { background: #181818; border-bottom: 1px solid #2a2a2a; }"
        "#appTitleBarTitle { color: rgb(131,193,224); font-weight: 600; }"
        "QPushButton { background: transparent; border: none; color: rgb(131,193,224);"
        "              border-radius: 4px; padding: 0; }"
        "QPushButton:hover { background: #2a2a2a; color: white; }"
        "QPushButton:pressed { background: #3a3a3a; color: white; }"
        "#appTitleBarClose:hover { background: #b23b4c; color: white; }"));
}

void AppTitleBar::toggleMaximize()
{
    if (!m_targetWindow)
        return;

    if (m_targetWindow->isMaximized())
        m_targetWindow->showNormal();
    else
        m_targetWindow->showMaximized();
    updateMaximizeButton();
}

void AppTitleBar::updateMaximizeButton()
{
    if (!m_targetWindow || !m_maximizeButton)
        return;

    m_maximizeButton->setIcon(makeTitleBarIcon(m_targetWindow->isMaximized()
                                                   ? TitleBarGlyph::Restore
                                                   : TitleBarGlyph::Maximize));
    m_maximizeButton->setToolTip(m_targetWindow->isMaximized() ? tr("Restore") : tr("Maximize"));
}

Qt::Edges AppTitleBar::hitTest(const QPoint &globalPos) const
{
    if (!m_targetWindow || !m_targetWindow->isVisible() || m_targetWindow->isMaximized())
        return Qt::Edges();

    const QRect rect = m_targetWindow->frameGeometry();
    if (!rect.adjusted(-4, -4, 4, 4).contains(globalPos))
        return Qt::Edges();

    constexpr int kBorder = 6;
    const bool nearX = globalPos.x() >= rect.left() - kBorder && globalPos.x() <= rect.right() + kBorder;
    const bool nearY = globalPos.y() >= rect.top() - kBorder && globalPos.y() <= rect.bottom() + kBorder;
    Qt::Edges edges;
    if (nearY && qAbs(globalPos.x() - rect.left()) <= kBorder)
        edges |= Qt::LeftEdge;
    if (nearY && qAbs(globalPos.x() - rect.right()) <= kBorder)
        edges |= Qt::RightEdge;
    if (nearX && qAbs(globalPos.y() - rect.top()) <= kBorder)
        edges |= Qt::TopEdge;
    if (nearX && qAbs(globalPos.y() - rect.bottom()) <= kBorder)
        edges |= Qt::BottomEdge;
    return edges;
}

void AppTitleBar::updateCursor(const QPoint &globalPos)
{
    if (!m_targetWindow || m_resizing || m_dragging)
        return;

    const Qt::Edges edges = hitTest(globalPos);
    Qt::CursorShape shape = Qt::ArrowCursor;
    if ((edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::TopEdge)) ||
        (edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::BottomEdge)))
    {
        shape = Qt::SizeFDiagCursor;
    }
    else if ((edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::TopEdge)) ||
             (edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::BottomEdge)))
    {
        shape = Qt::SizeBDiagCursor;
    }
    else if (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge))
    {
        shape = Qt::SizeHorCursor;
    }
    else if (edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge))
    {
        shape = Qt::SizeVerCursor;
    }

    if (shape == Qt::ArrowCursor)
        m_targetWindow->unsetCursor();
    else
        m_targetWindow->setCursor(shape);
}

QRect AppTitleBar::applyResizeAspectRatio(const QRect &geometry, const QPoint &globalPos) const
{
    Q_UNUSED(globalPos);
    if (!m_targetWindow || m_resizeAspectBase.isEmpty() || m_resizeAspectBase.height() <= 0)
        return geometry;

    const QSize minSize = m_targetWindow->minimumSize();
    const double aspect = m_resizeAspectBase.width() / static_cast<double>(m_resizeAspectBase.height());
    QRect adjusted = geometry;
    int width = qMax(minSize.width(), adjusted.width());
    int height = qMax(minSize.height(), static_cast<int>(qRound(width / aspect)));
    if (height < minSize.height())
    {
        height = minSize.height();
        width = qMax(minSize.width(), static_cast<int>(qRound(height * aspect)));
    }

    if (m_resizeEdges.testFlag(Qt::LeftEdge) && !m_resizeEdges.testFlag(Qt::RightEdge))
        adjusted.setLeft(adjusted.right() - width + 1);
    else
        adjusted.setWidth(width);

    if (m_resizeEdges.testFlag(Qt::TopEdge) && !m_resizeEdges.testFlag(Qt::BottomEdge))
        adjusted.setTop(adjusted.bottom() - height + 1);
    else
        adjusted.setHeight(height);

    if (m_resizeEdges == Qt::TopEdge || m_resizeEdges == Qt::BottomEdge)
    {
        height = qMax(minSize.height(), geometry.height());
        width = qMax(minSize.width(), static_cast<int>(qRound(height * aspect)));
        adjusted = geometry;
        if (m_resizeEdges.testFlag(Qt::TopEdge))
            adjusted.setTop(adjusted.bottom() - height + 1);
        else
            adjusted.setHeight(height);

        adjusted.moveLeft(geometry.center().x() - width / 2);
        adjusted.setWidth(width);
    }

    return adjusted;
}

bool AppTitleBar::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    if (!m_targetWindow || !m_targetWindow->isVisible())
        return QWidget::eventFilter(watched, event);

    if (event->type() != QEvent::MouseMove &&
        event->type() != QEvent::MouseButtonPress &&
        event->type() != QEvent::MouseButtonRelease)
    {
        return QWidget::eventFilter(watched, event);
    }

    auto *mouseEvent = static_cast<QMouseEvent *>(event);
    const QPoint globalPos = mouseEvent->globalPos();

    if (event->type() == QEvent::MouseButtonPress && mouseEvent->button() == Qt::LeftButton)
    {
        m_resizeEdges = hitTest(globalPos);
        if (!m_resizeEdges)
            return QWidget::eventFilter(watched, event);

        QWidget *eventWidget = qobject_cast<QWidget *>(watched);
        if (eventWidget && eventWidget != m_targetWindow && !m_targetWindow->isAncestorOf(eventWidget))
            return QWidget::eventFilter(watched, event);

        m_resizing = true;
        mouseEvent->accept();
        return true;
    }

    if (event->type() == QEvent::MouseMove)
    {
        if (m_resizing && (mouseEvent->buttons() & Qt::LeftButton))
        {
            QRect geometry = m_targetWindow->geometry();
            const QSize minSize = m_targetWindow->minimumSize();
            if (m_resizeEdges.testFlag(Qt::LeftEdge))
            {
                int left = qMin(globalPos.x(), geometry.right() - minSize.width() + 1);
                geometry.setLeft(left);
            }
            if (m_resizeEdges.testFlag(Qt::RightEdge))
            {
                int right = qMax(globalPos.x(), geometry.left() + minSize.width() - 1);
                geometry.setRight(right);
            }
            if (m_resizeEdges.testFlag(Qt::TopEdge))
            {
                int top = qMin(globalPos.y(), geometry.bottom() - minSize.height() + 1);
                geometry.setTop(top);
            }
            if (m_resizeEdges.testFlag(Qt::BottomEdge))
            {
                int bottom = qMax(globalPos.y(), geometry.top() + minSize.height() - 1);
                geometry.setBottom(bottom);
            }
            m_targetWindow->setGeometry(applyResizeAspectRatio(geometry, globalPos));
            mouseEvent->accept();
            return true;
        }
        updateCursor(globalPos);
    }

    if (event->type() == QEvent::MouseButtonRelease && m_resizing)
    {
        m_resizing = false;
        m_resizeEdges = Qt::Edges();
        updateCursor(globalPos);
        mouseEvent->accept();
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

void AppTitleBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_targetWindow)
    {
        m_dragging = true;
        m_dragOffset = event->globalPos() - m_targetWindow->frameGeometry().topLeft();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void AppTitleBar::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && m_targetWindow && (event->buttons() & Qt::LeftButton))
    {
        if (m_targetWindow->isMaximized())
        {
            m_targetWindow->showNormal();
            m_dragOffset = QPoint(m_targetWindow->width() / 2, height() / 2);
        }
        m_targetWindow->move(event->globalPos() - m_dragOffset);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void AppTitleBar::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    QWidget::mouseReleaseEvent(event);
}

void AppTitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_maximizeButton)
    {
        toggleMaximize();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
