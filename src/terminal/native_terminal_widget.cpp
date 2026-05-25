#include "terminal/native_terminal_widget.h"
#include "terminal/native_terminal_widget_colors.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QTimerEvent>
#include <QUrl>


QString NativeTerminalWidget::preferredTerminalFontFamily()
{
#if defined(Q_OS_WIN)
    const QFontDatabase database;
    const QStringList families = database.families();
    const QString candidates[] = {
        QStringLiteral("Microsoft YaHei Mono"),
        QStringLiteral("NSimSun"),
        QStringLiteral("SimSun"),
        QStringLiteral("Consolas")};
    for (const QString &candidate : candidates)
    {
        if (families.contains(candidate))
        {
            return candidate;
        }
    }
#endif
    return QString();
}

QString NativeTerminalWidget::pathFromOsc7Payload(const QByteArray &payload)
{
    const QString text = QString::fromUtf8(payload);
    const QUrl url(text);
    if (url.scheme() == QStringLiteral("file"))
    {
        QString path;
        const QString host = url.host().toLower();
        if (host.isEmpty() || host == QStringLiteral("localhost"))
            path = QUrl::fromPercentEncoding(url.path().toUtf8());
        else
            path = url.toLocalFile();

        if (!path.isEmpty())
        {
            if (path.size() >= 3 && path.at(0) == QLatin1Char('/') && path.at(2) == QLatin1Char(':'))
                path.remove(0, 1);
            return QDir::fromNativeSeparators(path);
        }
    }

    const QString prefix = QStringLiteral("file:///");
    if (text.startsWith(prefix, Qt::CaseInsensitive))
    {
        QString path = QUrl::fromPercentEncoding(text.mid(prefix.size()).toUtf8());
        if (path.size() >= 2 && path.at(1) == QLatin1Char(':'))
            return QDir::fromNativeSeparators(path);
        return QDir::fromNativeSeparators(QStringLiteral("/") + path);
    }
    return QString();
}

bool NativeTerminalWidget::isWideCharTrailingCell(const VTermScreenCell &cell)
{
    return cell.chars[0] == static_cast<uint32_t>(-1);
}

bool NativeTerminalWidget::isCellBefore(const QPoint &left, const QPoint &right)
{
    if (left.y() != right.y())
        return left.y() < right.y();
    return left.x() < right.x();
}

QString NativeTerminalWidget::trimTrailingSpaces(QString text)
{
    while (!text.isEmpty() && text.at(text.size() - 1).isSpace())
        text.chop(1);
    return text;
}

NativeTerminalWidget::NativeTerminalWidget(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled);

    const QString preferredFont = preferredTerminalFontFamily();
    m_font = preferredFont.isEmpty()
                 ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                 : QFont(preferredFont);
    m_font.setPointSize(11);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    setFont(m_font);

    QFontMetrics metrics(m_font);
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    m_cellWidth = qMax(1, metrics.horizontalAdvance(QLatin1Char('M')));
#else
    m_cellWidth = qMax(1, metrics.width(QLatin1Char('M')));
#endif
    m_cellHeight = qMax(1, metrics.lineSpacing());
    m_ascent = metrics.ascent();

    initializeTerminal(m_gridSize.width(), m_gridSize.height());
    updateCursorBlink();
}

NativeTerminalWidget::~NativeTerminalWidget()
{
    if (m_vterm)
        vterm_free(m_vterm);
}

QSize NativeTerminalWidget::gridSize() const
{
    return m_gridSize;
}

void NativeTerminalWidget::setLocalEchoEnabled(bool enabled)
{
    m_localEchoEnabled = enabled;
}

void NativeTerminalWidget::initializeTerminal(int cols, int rows)
{
    if (m_vterm)
        vterm_free(m_vterm);

    m_gridSize = QSize(qMax(20, cols), qMax(5, rows));
    m_vterm = vterm_new(m_gridSize.height(), m_gridSize.width());
    vterm_set_utf8(m_vterm, 1);
    vterm_output_set_callback(m_vterm, &NativeTerminalWidget::outputCallback, this);

    m_screen = vterm_obtain_screen(m_vterm);
    static const VTermScreenCallbacks callbacks = {
        &NativeTerminalWidget::damageCallback,
        nullptr,
        &NativeTerminalWidget::cursorCallback,
        &NativeTerminalWidget::termPropCallback,
        &NativeTerminalWidget::bellCallback,
        &NativeTerminalWidget::resizeCallback,
        &NativeTerminalWidget::scrollbackPushCallback,
        &NativeTerminalWidget::scrollbackPopCallback,
        &NativeTerminalWidget::scrollbackClearCallback};
    vterm_screen_set_callbacks(m_screen, &callbacks, this);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_enable_reflow(m_screen, true);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_ROW);

    VTermColor fg;
    VTermColor bg;
    vterm_color_rgb(&fg, kDefaultForeground.red(), kDefaultForeground.green(), kDefaultForeground.blue());
    vterm_color_rgb(&bg, kDefaultBackground.red(), kDefaultBackground.green(), kDefaultBackground.blue());
    vterm_screen_set_default_colors(m_screen, &fg, &bg);
    vterm_screen_reset(m_screen, 1);
}

void NativeTerminalWidget::writePtyOutput(const QByteArray &data)
{
    if (!m_vterm || data.isEmpty())
        return;

    parseOsc7Directory(data);
    vterm_input_write(m_vterm, data.constData(), static_cast<size_t>(data.size()));
    flushDamage();
}

void NativeTerminalWidget::showStatusLine(const QString &message)
{
    writePtyOutput((QStringLiteral("\r\n") + message + QStringLiteral("\r\n")).toUtf8());
}

void NativeTerminalWidget::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_blinkTimer.timerId())
    {
        m_cursorBlinkState = !m_cursorBlinkState;
        const QRect cursorRect(m_cursorPos.col * m_cellWidth, m_cursorPos.row * m_cellHeight, m_cellWidth, m_cellHeight);
        update(cursorRect);
        return;
    }
    QWidget::timerEvent(event);
}

void NativeTerminalWidget::updateGridFromViewport()
{
    const int cols = qMax(20, width() / m_cellWidth);
    const int rows = qMax(5, height() / m_cellHeight);
    if (cols == m_gridSize.width() && rows == m_gridSize.height())
        return;

    m_gridSize = QSize(cols, rows);
    if (m_vterm)
    {
        vterm_set_size(m_vterm, rows, cols);
        flushDamage();
    }
    emit gridSizeChanged(m_gridSize);
}

void NativeTerminalWidget::sendKey(VTermKey key, VTermModifier modifiers)
{
    vterm_keyboard_key(m_vterm, key, modifiers);
}

void NativeTerminalWidget::sendText(const QString &text, VTermModifier modifiers)
{
    if (modifiers == VTERM_MOD_NONE || modifiers == VTERM_MOD_SHIFT)
    {
        const QByteArray bytes = terminalInputBytesFromText(text);
        handleOutputBytes(bytes.constData(), static_cast<size_t>(bytes.size()));
        return;
    }

    for (const uint ucs4 : text.toUcs4())
    {
        if (ucs4 < 0x20 || ucs4 == 0x7f)
        {
            const QByteArray bytes = terminalInputBytesFromText(text);
            handleOutputBytes(bytes.constData(), static_cast<size_t>(bytes.size()));
            return;
        }
    }

    for (const uint ucs4 : text.toUcs4())
        vterm_keyboard_unichar(m_vterm, ucs4, modifiers);
}

void NativeTerminalWidget::sendClipboardPaste()
{
    const QString text = QApplication::clipboard()->text();
    if (text.isEmpty())
        return;

    const QByteArray bytes = terminalInputBytesFromText(text);
    vterm_keyboard_start_paste(m_vterm);
    handleOutputBytes(bytes.constData(), static_cast<size_t>(bytes.size()));
    vterm_keyboard_end_paste(m_vterm);
}

QByteArray NativeTerminalWidget::terminalInputBytesFromText(const QString &text) const
{
    QByteArray bytes;
    QString pendingText;
    for (int i = 0; i < text.size(); ++i)
    {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('\r'))
        {
            if (!pendingText.isEmpty())
            {
                bytes.append(pendingText.toUtf8());
                pendingText.clear();
            }
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n'))
                ++i;
            bytes.append('\r');
        }
        else if (ch == QLatin1Char('\n'))
        {
            if (!pendingText.isEmpty())
            {
                bytes.append(pendingText.toUtf8());
                pendingText.clear();
            }
            bytes.append('\r');
        }
        else
        {
            pendingText.append(ch);
        }
    }
    if (!pendingText.isEmpty())
        bytes.append(pendingText.toUtf8());
    return bytes;
}

QByteArray NativeTerminalWidget::localEchoBytes(const char *data, size_t len) const
{
    QByteArray source(data, static_cast<int>(len));
    if (source.contains('\x1b'))
        return QByteArray();

    QString text = QString::fromUtf8(source);
    QByteArray echo;
    for (int i = 0; i < text.size(); ++i)
    {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n'))
        {
            echo.append("\r\n");
        }
        else if (ch == QLatin1Char('\b') || ch.unicode() == 0x7f)
        {
            echo.append("\b \b");
        }
        else if (ch == QLatin1Char('\t') || ch.unicode() >= 0x20)
        {
            echo.append(QString(ch).toUtf8());
        }
    }
    return echo;
}

void NativeTerminalWidget::flushDamage()
{
    vterm_screen_flush_damage(m_screen);
}

void NativeTerminalWidget::handleOutputBytes(const char *data, size_t len)
{
    if (m_scrollbackOffset > 0)
    {
        m_scrollbackOffset = 0;
        update();
    }
    if (m_localEchoEnabled && m_vterm && len > 0)
    {
        const QByteArray echo = localEchoBytes(data, len);
        if (!echo.isEmpty())
        {
            vterm_input_write(m_vterm, echo.constData(), static_cast<size_t>(echo.size()));
            flushDamage();
        }
    }
    if (len > 0)
        emit inputGenerated(QByteArray(data, static_cast<int>(len)));
}

void NativeTerminalWidget::updateCursorBlink()
{
    if (m_cursorBlink && hasFocus())
    {
        if (!m_blinkTimer.isActive())
            m_blinkTimer.start(530, this);
    }
    else
    {
        m_blinkTimer.stop();
        m_cursorBlinkState = true;
    }
}

void NativeTerminalWidget::parseOsc7Directory(const QByteArray &data)
{
    int pos = 0;
    while ((pos = data.indexOf("\x1b]7;", pos)) >= 0)
    {
        const int start = pos + 4;
        int end = data.indexOf('\x07', start);
        int terminatorLength = 1;
        const int stEnd = data.indexOf("\x1b\\", start);
        if (end < 0 || (stEnd >= 0 && stEnd < end))
        {
            end = stEnd;
            terminatorLength = 2;
        }
        if (end < 0)
            return;

        const QString path = pathFromOsc7Payload(data.mid(start, end - start));
        if (!path.isEmpty())
            emit currentDirectoryChanged(path);
        pos = end + terminatorLength;
    }
}
