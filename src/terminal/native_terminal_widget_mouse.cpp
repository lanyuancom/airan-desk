#include "terminal/native_terminal_widget.h"
#include "util/config_util.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>

void NativeTerminalWidget::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);

    if (event->button() == Qt::RightButton)
    {
        showContextMenu(event->globalPos());
        event->accept();
        return;
    }

    if (shouldStartLocalSelection(event))
    {
        beginSelection(cellFromPosition(event->pos()));
        event->accept();
        return;
    }

    sendMouseButton(event, true);
}

void NativeTerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_selecting)
    {
        updateSelection(cellFromPosition(event->pos()));
        finishSelection();
        event->accept();
        return;
    }

    sendMouseButton(event, false);
}

void NativeTerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_selecting)
    {
        updateSelection(cellFromPosition(event->pos()));
        event->accept();
        return;
    }

    sendMouseMove(event);
}

void NativeTerminalWidget::wheelEvent(QWheelEvent *event)
{
    if (!m_vterm)
    {
        QWidget::wheelEvent(event);
        return;
    }

    const bool forceLocalScroll = event->modifiers().testFlag(Qt::ShiftModifier);
    if (m_mouseMode == VTERM_PROP_MOUSE_NONE || forceLocalScroll)
    {
        int steps = event->angleDelta().y() / 120;
        if (steps == 0)
            steps = event->pixelDelta().y() / qMax(1, m_cellHeight);
        if (steps != 0)
        {
            scrollHistory(steps * 3);
            event->accept();
            return;
        }
    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QPoint cell = cellFromPosition(event->position().toPoint());
#else
    const QPoint cell = cellFromPosition(event->pos());
#endif
    vterm_mouse_move(m_vterm, cell.y(), cell.x(), mouseModifiersFromQt(event->modifiers()));
    const int button = event->angleDelta().y() > 0 ? 4 : 5;
    vterm_mouse_button(m_vterm, button, true, mouseModifiersFromQt(event->modifiers()));
    vterm_mouse_button(m_vterm, button, false, mouseModifiersFromQt(event->modifiers()));
    event->accept();
}

bool NativeTerminalWidget::clipboardHasText() const
{
    const QClipboard *clipboard = QApplication::clipboard();
    return clipboard && !clipboard->text().isEmpty();
}

void NativeTerminalWidget::showContextMenu(const QPoint &globalPos)
{
    if (!ConfigUtil->showUI)
        return;

    QMenu menu(this);
    QAction *copyAction = menu.addAction(tr("复制"));
    QAction *pasteAction = menu.addAction(tr("粘贴"));
    copyAction->setEnabled(hasSelection());
    pasteAction->setEnabled(clipboardHasText());
    connect(copyAction, &QAction::triggered, this, &NativeTerminalWidget::copySelectionToClipboard);
    connect(pasteAction, &QAction::triggered, this, &NativeTerminalWidget::sendClipboardPaste);
    menu.exec(globalPos);
}

void NativeTerminalWidget::sendMouseButton(QMouseEvent *event, bool pressed)
{
    if (!m_vterm || m_mouseMode == VTERM_PROP_MOUSE_NONE)
    {
        event->ignore();
        return;
    }

    int button = 0;
    if (event->button() == Qt::LeftButton)
        button = 1;
    else if (event->button() == Qt::MiddleButton)
        button = 2;
    else if (event->button() == Qt::RightButton)
        button = 3;
    else
        return;

    const QPoint cell = cellFromPosition(event->pos());
    vterm_mouse_move(m_vterm, cell.y(), cell.x(), mouseModifiersFromQt(event->modifiers()));
    vterm_mouse_button(m_vterm, button, pressed, mouseModifiersFromQt(event->modifiers()));
    event->accept();
}

void NativeTerminalWidget::sendMouseMove(QMouseEvent *event)
{
    if (!m_vterm || m_mouseMode == VTERM_PROP_MOUSE_NONE)
        return;

    const QPoint cell = cellFromPosition(event->pos());
    vterm_mouse_move(m_vterm, cell.y(), cell.x(), mouseModifiersFromQt(event->modifiers()));
}

QPoint NativeTerminalWidget::cellFromPosition(const QPoint &pos) const
{
    const int col = qBound(0, pos.x() / m_cellWidth, m_gridSize.width() - 1);
    const int row = qBound(0, pos.y() / m_cellHeight, m_gridSize.height() - 1);
    return QPoint(col, row);
}

void NativeTerminalWidget::scrollHistory(int lines)
{
    if (m_scrollback.isEmpty() || lines == 0)
        return;

    const int newOffset = qBound(0, m_scrollbackOffset + lines, m_scrollback.size());
    if (newOffset == m_scrollbackOffset)
        return;

    clearSelection();
    m_scrollbackOffset = newOffset;
    update();
}

bool NativeTerminalWidget::shouldStartLocalSelection(QMouseEvent *event) const
{
    if (event->button() != Qt::LeftButton)
        return false;
    return m_mouseMode == VTERM_PROP_MOUSE_NONE || event->modifiers().testFlag(Qt::ShiftModifier);
}

void NativeTerminalWidget::beginSelection(const QPoint &cell)
{
    m_selecting = true;
    m_hasSelection = true;
    m_selectionAnchor = cell;
    m_selectionCursor = cell;
    update();
}

void NativeTerminalWidget::updateSelection(const QPoint &cell)
{
    if (m_selectionCursor == cell)
        return;

    m_selectionCursor = cell;
    m_hasSelection = true;
    update();
}

void NativeTerminalWidget::finishSelection()
{
    m_selecting = false;
    if (m_selectionAnchor == m_selectionCursor)
        m_hasSelection = false;
    update();
}

void NativeTerminalWidget::clearSelection()
{
    if (!m_hasSelection && !m_selecting)
        return;

    m_selecting = false;
    m_hasSelection = false;
    update();
}

bool NativeTerminalWidget::hasSelection() const
{
    return m_hasSelection && m_selectionAnchor != m_selectionCursor;
}

bool NativeTerminalWidget::isCellSelected(int row, int col) const
{
    if (!hasSelection())
        return false;

    QPoint start = m_selectionAnchor;
    QPoint end = m_selectionCursor;
    if (isCellBefore(end, start))
        std::swap(start, end);

    if (row < start.y() || row > end.y())
        return false;
    if (row == start.y() && col < start.x())
        return false;
    if (row == end.y() && col > end.x())
        return false;
    return true;
}

bool NativeTerminalWidget::isCellRangeSelected(int row, int col, int width) const
{
    const int lastCol = col + qMax(1, width) - 1;
    for (int currentCol = col; currentCol <= lastCol; ++currentCol)
    {
        if (isCellSelected(row, currentCol))
            return true;
    }
    return false;
}

QString NativeTerminalWidget::selectedText() const
{
    if (!m_screen || !hasSelection())
        return QString();

    QPoint start = m_selectionAnchor;
    QPoint end = m_selectionCursor;
    if (isCellBefore(end, start))
        std::swap(start, end);

    QString result;
    VTermScreenCell cell{};
    for (int row = start.y(); row <= end.y(); ++row)
    {
        QString line;
        const int startCol = row == start.y() ? start.x() : 0;
        const int endCol = row == end.y() ? end.x() : m_gridSize.width() - 1;
        for (int col = startCol; col <= endCol; ++col)
        {
            if (!visibleCell(row, col, &cell))
                continue;
            if (cell.width == 0 || isWideCharTrailingCell(cell))
                continue;
            line += cellText(cell);
        }

        result += trimTrailingSpaces(line);
        if (row != end.y())
            result += QLatin1Char('\n');
    }
    return result;
}

void NativeTerminalWidget::copySelectionToClipboard()
{
    const QString text = selectedText();
    if (text.isEmpty())
        return;
    QApplication::clipboard()->setText(text);
}
