#include "terminal/native_terminal_widget.h"

#include <QApplication>
#include <QVector>

void NativeTerminalWidget::outputCallback(const char *data, size_t len, void *user)
{
    static_cast<NativeTerminalWidget *>(user)->handleOutputBytes(data, len);
}

int NativeTerminalWidget::damageCallback(VTermRect rect, void *user)
{
    auto *self = static_cast<NativeTerminalWidget *>(user);
    if (self->m_scrollbackOffset > 0)
    {
        self->update();
        return 1;
    }

    self->update(QRect(rect.start_col * self->m_cellWidth,
                       rect.start_row * self->m_cellHeight,
                       (rect.end_col - rect.start_col) * self->m_cellWidth,
                       (rect.end_row - rect.start_row) * self->m_cellHeight));
    return 1;
}

int NativeTerminalWidget::cursorCallback(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    auto *self = static_cast<NativeTerminalWidget *>(user);
    self->m_cursorPos = pos;
    self->m_cursorVisible = visible;
    self->update(QRect(oldpos.col * self->m_cellWidth, oldpos.row * self->m_cellHeight, self->m_cellWidth, self->m_cellHeight));
    self->update(QRect(pos.col * self->m_cellWidth, pos.row * self->m_cellHeight, self->m_cellWidth, self->m_cellHeight));
    return 1;
}

int NativeTerminalWidget::termPropCallback(VTermProp prop, VTermValue *val, void *user)
{
    auto *self = static_cast<NativeTerminalWidget *>(user);
    switch (prop)
    {
    case VTERM_PROP_CURSORVISIBLE:
        self->m_cursorVisible = val->boolean;
        self->update();
        return 1;
    case VTERM_PROP_CURSORBLINK:
        self->m_cursorBlink = val->boolean;
        self->updateCursorBlink();
        return 1;
    case VTERM_PROP_CURSORSHAPE:
        self->m_cursorShape = val->number;
        self->update();
        return 1;
    case VTERM_PROP_MOUSE:
        self->m_mouseMode = val->number;
        return 1;
    case VTERM_PROP_FOCUSREPORT:
        self->m_focusReport = val->boolean;
        return 1;
    case VTERM_PROP_TITLE:
        emit self->terminalTitleChanged(QString::fromUtf8(val->string.str, static_cast<int>(val->string.len)));
        return 1;
    default:
        return 1;
    }
}

int NativeTerminalWidget::bellCallback(void *user)
{
    emit static_cast<NativeTerminalWidget *>(user)->bell();
    QApplication::beep();
    return 1;
}

int NativeTerminalWidget::resizeCallback(int rows, int cols, void *user)
{
    auto *self = static_cast<NativeTerminalWidget *>(user);
    self->m_gridSize = QSize(cols, rows);
    self->m_scrollbackOffset = qBound(0, self->m_scrollbackOffset, self->m_scrollback.size());
    self->update();
    return 1;
}

int NativeTerminalWidget::scrollbackPushCallback(int cols, const VTermScreenCell *cells, void *user)
{
    auto *self = static_cast<NativeTerminalWidget *>(user);
    if (!self || cols <= 0 || !cells)
        return 1;

    QVector<VTermScreenCell> line;
    line.reserve(cols);
    for (int col = 0; col < cols; ++col)
        line.append(cells[col]);

    self->m_scrollback.append(line);
    if (self->m_scrollbackOffset > 0)
        self->m_scrollbackOffset = qMin(self->m_scrollbackOffset + 1, self->m_scrollback.size());
    return 1;
}

int NativeTerminalWidget::scrollbackPopCallback(int cols, VTermScreenCell *cells, void *user)
{
    auto *self = static_cast<NativeTerminalWidget *>(user);
    if (!self || cols <= 0 || !cells || self->m_scrollback.isEmpty())
        return 0;

    const QVector<VTermScreenCell> line = self->m_scrollback.takeLast();
    for (int col = 0; col < cols; ++col)
        cells[col] = col < line.size() ? line.at(col) : VTermScreenCell{};

    self->m_scrollbackOffset = qBound(0, self->m_scrollbackOffset, self->m_scrollback.size());
    return 1;
}

int NativeTerminalWidget::scrollbackClearCallback(void *user)
{
    auto *self = static_cast<NativeTerminalWidget *>(user);
    if (self)
    {
        self->m_scrollback.clear();
        self->m_scrollbackOffset = 0;
        self->clearSelection();
    }
    return 1;
}
