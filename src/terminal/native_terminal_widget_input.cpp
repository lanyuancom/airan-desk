#include "terminal/native_terminal_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QResizeEvent>

void NativeTerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateGridFromViewport();
}

void NativeTerminalWidget::keyPressEvent(QKeyEvent *event)
{
    if (!m_vterm)
        return;

    if (event->matches(QKeySequence::Paste) ||
        (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_V))
    {
        sendClipboardPaste();
        event->accept();
        return;
    }

    const VTermModifier modifiers = modifiersFromQt(event->modifiers());
    VTermKey key = VTERM_KEY_NONE;
    switch (event->key())
    {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        key = VTERM_KEY_ENTER;
        break;
    case Qt::Key_Tab:
        key = VTERM_KEY_TAB;
        break;
    case Qt::Key_Backspace:
        key = VTERM_KEY_BACKSPACE;
        break;
    case Qt::Key_Escape:
        key = VTERM_KEY_ESCAPE;
        break;
    case Qt::Key_Up:
        key = VTERM_KEY_UP;
        break;
    case Qt::Key_Down:
        key = VTERM_KEY_DOWN;
        break;
    case Qt::Key_Left:
        key = VTERM_KEY_LEFT;
        break;
    case Qt::Key_Right:
        key = VTERM_KEY_RIGHT;
        break;
    case Qt::Key_Insert:
        key = VTERM_KEY_INS;
        break;
    case Qt::Key_Delete:
        key = VTERM_KEY_DEL;
        break;
    case Qt::Key_Home:
        key = VTERM_KEY_HOME;
        break;
    case Qt::Key_End:
        key = VTERM_KEY_END;
        break;
    case Qt::Key_PageUp:
        key = VTERM_KEY_PAGEUP;
        break;
    case Qt::Key_PageDown:
        key = VTERM_KEY_PAGEDOWN;
        break;
    default:
        if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F12)
            key = static_cast<VTermKey>(VTERM_KEY_FUNCTION(event->key() - Qt::Key_F1 + 1));
        break;
    }

    if (key != VTERM_KEY_NONE)
    {
        sendKey(key, modifiers);
        event->accept();
        return;
    }

    if (!event->text().isEmpty())
    {
        sendText(event->text(), modifiers);
        event->accept();
        return;
    }

    QWidget::keyPressEvent(event);
}

void NativeTerminalWidget::inputMethodEvent(QInputMethodEvent *event)
{
    if (!m_vterm)
    {
        QWidget::inputMethodEvent(event);
        return;
    }

    const QString commitText = event->commitString();
    if (!commitText.isEmpty())
    {
        clearSelection();
        sendText(commitText, VTERM_MOD_NONE);
        event->accept();
        return;
    }

    QWidget::inputMethodEvent(event);
}

QVariant NativeTerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImCursorRectangle)
        return QRect(m_cursorPos.col * m_cellWidth, m_cursorPos.row * m_cellHeight, m_cellWidth, m_cellHeight);
    if (query == Qt::ImFont)
        return m_font;
    return QWidget::inputMethodQuery(query);
}

void NativeTerminalWidget::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    if (m_vterm && m_focusReport)
        handleOutputBytes("\x1b[I", 3);
    m_cursorBlinkState = true;
    updateCursorBlink();
    update();
}

void NativeTerminalWidget::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    if (m_vterm && m_focusReport)
        handleOutputBytes("\x1b[O", 3);
    updateCursorBlink();
    update();
}



VTermModifier NativeTerminalWidget::modifiersFromQt(Qt::KeyboardModifiers modifiers) const
{
    int result = VTERM_MOD_NONE;
    if (modifiers.testFlag(Qt::ShiftModifier))
        result |= VTERM_MOD_SHIFT;
    if (modifiers.testFlag(Qt::AltModifier))
        result |= VTERM_MOD_ALT;
    if (modifiers.testFlag(Qt::ControlModifier))
        result |= VTERM_MOD_CTRL;
    return static_cast<VTermModifier>(result);
}

VTermModifier NativeTerminalWidget::mouseModifiersFromQt(Qt::KeyboardModifiers modifiers) const
{
    return modifiersFromQt(modifiers);
}


