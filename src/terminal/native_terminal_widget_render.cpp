#include "terminal/native_terminal_widget.h"
#include "terminal/native_terminal_widget_colors.h"

#include <QPainter>
#include <QPaintEvent>
#include <algorithm>

void NativeTerminalWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), kDefaultBackground);
    painter.setFont(m_font);

    const int firstRow = qMax(0, event->rect().top() / m_cellHeight);
    const int lastRow = qMin(m_gridSize.height() - 1, event->rect().bottom() / m_cellHeight);
    VTermScreenCell cell{};

    for (int row = firstRow; row <= lastRow; ++row)
    {
        for (int col = 0; col < m_gridSize.width(); ++col)
        {
            if (!visibleCell(row, col, &cell))
                continue;
            if (cell.width == 0 || isWideCharTrailingCell(cell))
                continue;

            QColor fg = colorFromVTerm(cell.fg, true);
            QColor bg = colorFromVTerm(cell.bg, false);
            if (cell.attrs.reverse)
                std::swap(fg, bg);
            if (cell.attrs.conceal)
                fg = bg;

            const QRect cellRect(col * m_cellWidth, row * m_cellHeight, m_cellWidth * qMax(1, static_cast<int>(cell.width)), m_cellHeight);
            if (isCellRangeSelected(row, col, qMax(1, static_cast<int>(cell.width))))
                bg = kSelectionBackground;
            painter.fillRect(cellRect, bg);
        }

        for (int col = 0; col < m_gridSize.width(); ++col)
        {
            if (!visibleCell(row, col, &cell))
                continue;
            if (cell.width == 0)
                continue;

            QColor fg = colorFromVTerm(cell.fg, true);
            QColor bg = colorFromVTerm(cell.bg, false);
            if (cell.attrs.reverse)
                std::swap(fg, bg);
            if (cell.attrs.conceal)
                fg = bg;

            const QString text = cellText(cell);
            if (!text.isEmpty())
            {
                const QRect cellRect(col * m_cellWidth, row * m_cellHeight, m_cellWidth * qMax(1, static_cast<int>(cell.width)), m_cellHeight);
                if (isCellRangeSelected(row, col, qMax(1, static_cast<int>(cell.width))))
                    fg = kSelectionForeground;
                QFont drawFont = m_font;
                drawFont.setBold(cell.attrs.bold);
                drawFont.setItalic(cell.attrs.italic);
                drawFont.setStrikeOut(cell.attrs.strike);
                drawFont.setUnderline(cell.attrs.underline != VTERM_UNDERLINE_OFF);
                painter.setFont(drawFont);
                painter.setPen(fg);
                painter.drawText(cellRect.left(), cellRect.top() + m_ascent, text);
            }
        }
    }

    if (m_scrollbackOffset == 0 && hasFocus() && m_cursorVisible && m_cursorBlinkState)
    {
        const QRect cursorRect(m_cursorPos.col * m_cellWidth, m_cursorPos.row * m_cellHeight, m_cellWidth, m_cellHeight);
        if (m_cursorShape == VTERM_PROP_CURSORSHAPE_UNDERLINE)
        {
            painter.fillRect(QRect(cursorRect.left(), cursorRect.bottom() - 2, cursorRect.width(), 3), kCursorColor);
        }
        else if (m_cursorShape == VTERM_PROP_CURSORSHAPE_BAR_LEFT)
        {
            painter.fillRect(QRect(cursorRect.left(), cursorRect.top(), qMax(2, cursorRect.width() / 6), cursorRect.height()), kCursorColor);
        }
        else
        {
            painter.setCompositionMode(QPainter::RasterOp_SourceXorDestination);
            painter.fillRect(cursorRect, kCursorColor);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
    }
}

bool NativeTerminalWidget::visibleCell(int row, int col, VTermScreenCell *cell) const
{
    if (!cell)
        return false;

    const int globalLine = visibleGlobalLine(row);
    if (globalLine < m_scrollback.size())
    {
        const QVector<VTermScreenCell> &historyLine = m_scrollback.at(globalLine);
        if (col < 0 || col >= historyLine.size())
        {
            *cell = VTermScreenCell{};
            return true;
        }
        *cell = historyLine.at(col);
        return true;
    }

    if (!m_screen)
        return false;

    const int screenRow = globalLine - m_scrollback.size();
    if (screenRow < 0 || screenRow >= m_gridSize.height())
        return false;
    VTermPos pos{screenRow, col};
    return vterm_screen_get_cell(m_screen, pos, cell);
}

int NativeTerminalWidget::visibleGlobalLine(int row) const
{
    const int totalRows = m_scrollback.size() + m_gridSize.height();
    if (totalRows <= 0)
        return 0;

    const int startLine = qMax(0, totalRows - m_gridSize.height() - m_scrollbackOffset);
    return qBound(0, startLine + row, totalRows - 1);
}

QColor NativeTerminalWidget::colorFromVTerm(VTermColor color, bool foreground) const
{
    if (foreground && VTERM_COLOR_IS_DEFAULT_FG(&color))
        return kDefaultForeground;
    if (!foreground && VTERM_COLOR_IS_DEFAULT_BG(&color))
        return kDefaultBackground;

    vterm_screen_convert_color_to_rgb(m_screen, &color);
    return QColor(color.rgb.red, color.rgb.green, color.rgb.blue);
}

QString NativeTerminalWidget::cellText(const VTermScreenCell &cell) const
{
    if (isWideCharTrailingCell(cell))
        return QString();

    QVector<uint> chars;
    chars.reserve(VTERM_MAX_CHARS_PER_CELL);
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i)
        chars.append(cell.chars[i]);
    return chars.isEmpty() ? QString() : QString::fromUcs4(chars.constData(), chars.size());
}
