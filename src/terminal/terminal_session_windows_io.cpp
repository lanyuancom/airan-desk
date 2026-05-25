#include "terminal_session.h"
#include "common/logger_manager.h"

#include <QTextCodec>
#include <QTimer>

namespace
{
    QString terminalSessionPreview(const QByteArray &data)
    {
        QString preview = QString::fromUtf8(data);
        preview.replace(QStringLiteral("\r"), QStringLiteral("\\r"));
        preview.replace(QStringLiteral("\n"), QStringLiteral("\\n"));
        preview.replace(QStringLiteral("\t"), QStringLiteral("\\t"));
        if (preview.size() > 80)
            preview = preview.left(80) + QStringLiteral("...");
        return preview;
    }

    QString terminalSessionHexPreview(const QByteArray &data)
    {
        QString preview;
        const int count = qMin(data.size(), 32);
        preview.reserve(count * 3);
        for (int i = 0; i < count; ++i)
        {
            if (i > 0)
                preview += QLatin1Char(' ');
            preview += QStringLiteral("%1").arg(static_cast<unsigned char>(data.at(i)), 2, 16, QLatin1Char('0'));
        }
        if (data.size() > count)
            preview += QStringLiteral(" ...");
        return preview;
    }
}

#if defined(Q_OS_WIN)
QString TerminalSession::windowsConPtyShellNativeArguments() const
{
    return QStringLiteral("/D /K prompt $P$G");
}

QString TerminalSession::windowsFallbackShellNativeArguments() const
{
    return QStringLiteral("/D /K prompt $P$G");
}

QByteArray TerminalSession::normalizeFallbackInput(const QByteArray &data) const
{
    QByteArray normalized;
    normalized.reserve(data.size() + 8);
    for (int i = 0; i < data.size(); ++i)
    {
        const char ch = data.at(i);
        if (ch == '\r')
        {
            normalized.append("\r\n");
        }
        else if (ch == '\n')
        {
            normalized.append("\r\n");
        }
        else if (static_cast<unsigned char>(ch) == 0x7f)
        {
            normalized.append('\b');
        }
        else
        {
            normalized.append(ch);
        }
    }
    return encodeWindowsConsoleBytes(QString::fromUtf8(normalized));
}

QByteArray TerminalSession::normalizeWindowsInput(const QByteArray &data) const
{
    QByteArray normalized;
    normalized.reserve(data.size() + 8);
    for (int i = 0; i < data.size(); ++i)
    {
        const char ch = data.at(i);
        if (ch == '\r')
        {
            if (i + 1 < data.size() && data.at(i + 1) == '\n')
                ++i;
            normalized.append('\r');
        }
        else if (ch == '\n')
        {
            normalized.append('\r');
        }
        else if (static_cast<unsigned char>(ch) == 0x7f)
        {
            normalized.append('\b');
        }
        else
        {
            normalized.append(ch);
        }
    }
    return normalized;
}

DWORD TerminalSession::writeWindowsConPtyInput(const QByteArray &data)
{
    if (!m_inputWrite || data.isEmpty())
        return 0;

    DWORD written = 0;
    if (!WriteFile(m_inputWrite, data.constData(), static_cast<DWORD>(data.size()), &written, nullptr))
    {
        LOG_WARN("Failed to write terminal input to ConPTY: error={}", static_cast<int>(GetLastError()));
        return 0;
    }
    return written;
}

bool TerminalSession::windowsInputNeedsImmediateFlush(const QByteArray &data) const
{
    for (char ch : data)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f)
            return true;
    }
    return false;
}

void TerminalSession::flushWindowsPendingInput()
{
    if (m_windowsInputFlushTimer)
        m_windowsInputFlushTimer->stop();
    if (m_windowsPendingInput.isEmpty() || !m_inputWrite)
        return;

    const QByteArray input = m_windowsPendingInput;
    m_windowsPendingInput.clear();
    const DWORD written = writeWindowsConPtyInput(input);
    LOG_INFO("Terminal ConPTY input written: size={}, written={}, text={}, hex={}",
             input.size(),
             written,
             terminalSessionPreview(input),
             terminalSessionHexPreview(input));
}

QByteArray TerminalSession::decodeWindowsOutput(const char *data, int size)
{
    QByteArray bytes = m_windowsUtf8DecodePending + QByteArray(data, size);
    m_windowsUtf8DecodePending.clear();

    const int pendingUtf8Bytes = incompleteUtf8TailLength(bytes);
    const QByteArray utf8Candidate = pendingUtf8Bytes > 0
                                         ? bytes.left(bytes.size() - pendingUtf8Bytes)
                                         : bytes;

    if (!hasInvalidUtf8(utf8Candidate))
    {
        if (pendingUtf8Bytes > 0)
            m_windowsUtf8DecodePending = bytes.right(pendingUtf8Bytes);
        return utf8Candidate;
    }

    m_windowsUtf8DecodePending.clear();
    return decodeWindowsConsoleBytes(bytes);
}

QByteArray TerminalSession::decodeWindowsConsoleBytes(const QByteArray &data)
{
    QByteArray bytes = m_windowsConsoleDecodePending + data;
    m_windowsConsoleDecodePending.clear();

    const UINT codePage = GetOEMCP();
    if (!bytes.isEmpty() && IsDBCSLeadByteEx(codePage, static_cast<BYTE>(bytes.at(bytes.size() - 1))))
    {
        m_windowsConsoleDecodePending.append(bytes.at(bytes.size() - 1));
        bytes.chop(1);
    }

    if (bytes.isEmpty())
    {
        return QByteArray();
    }

    const int wideSize = MultiByteToWideChar(codePage, 0, bytes.constData(), bytes.size(), nullptr, 0);
    if (wideSize <= 0)
    {
        return QString::fromLocal8Bit(bytes).toUtf8();
    }

    std::wstring wide(static_cast<size_t>(wideSize), L'\0');
    MultiByteToWideChar(codePage, 0, bytes.constData(), bytes.size(), &wide[0], wideSize);
    return QString::fromWCharArray(wide.data(), wideSize).toUtf8();
}

QByteArray TerminalSession::encodeWindowsConsoleBytes(const QString &text) const
{
    if (text.isEmpty())
    {
        return QByteArray();
    }

    const UINT codePage = GetOEMCP();
    const std::wstring wide = text.toStdWString();
    const int byteSize = WideCharToMultiByte(codePage, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (byteSize <= 0)
    {
        return text.toLocal8Bit();
    }

    QByteArray bytes(byteSize, Qt::Uninitialized);
    WideCharToMultiByte(codePage, 0, wide.data(), static_cast<int>(wide.size()), bytes.data(), byteSize, nullptr, nullptr);
    return bytes;
}

bool TerminalSession::hasInvalidUtf8(const QByteArray &data) const
{
    QTextCodec::ConverterState state;
    QTextCodec *utf8 = QTextCodec::codecForName("UTF-8");
    if (!utf8)
    {
        return false;
    }
    utf8->toUnicode(data.constData(), data.size(), &state);
    if (state.invalidChars > 0)
    {
        return true;
    }
    if (!data.isEmpty())
    {
        const unsigned char last = static_cast<unsigned char>(data.at(data.size() - 1));
        return last >= 0xC2 && last <= 0xF4;
    }
    return false;
}

int TerminalSession::incompleteUtf8TailLength(const QByteArray &data) const
{
    if (data.isEmpty())
        return 0;

    int leadIndex = data.size() - 1;
    while (leadIndex >= 0)
    {
        const unsigned char byte = static_cast<unsigned char>(data.at(leadIndex));
        if (byte < 0x80 || byte > 0xBF)
            break;
        --leadIndex;
    }

    if (leadIndex < 0)
        return 0;

    const unsigned char lead = static_cast<unsigned char>(data.at(leadIndex));
    int expectedBytes = 0;
    if (lead >= 0xC2 && lead <= 0xDF)
        expectedBytes = 2;
    else if (lead >= 0xE0 && lead <= 0xEF)
        expectedBytes = 3;
    else if (lead >= 0xF0 && lead <= 0xF4)
        expectedBytes = 4;
    else
        return 0;

    const int availableBytes = data.size() - leadIndex;
    if (availableBytes < expectedBytes)
        return availableBytes;
    return 0;
}

void TerminalSession::logWindowsTerminalBytes(const QByteArray &data, const QString &source)
{
    if (data.isEmpty())
    {
        return;
    }

    bool hasHighBitByte = false;
    for (char ch : data)
    {
        if (static_cast<unsigned char>(ch) >= 0x80)
        {
            hasHighBitByte = true;
            break;
        }
    }

    if (m_windowsOutputLogCount >= 20 && !hasHighBitByte)
    {
        return;
    }
    ++m_windowsOutputLogCount;

    QTextCodec::ConverterState utf8State;
    QTextCodec *utf8 = QTextCodec::codecForName("UTF-8");
    if (utf8)
        utf8->toUnicode(data.constData(), data.size(), &utf8State);

    LOG_TRACE("Terminal Windows output sample source={}, size={}, utf8Invalid={}, selected={}",
              source,
              data.size(),
              utf8State.invalidChars,
              utf8State.invalidChars > 0 ? QStringLiteral("local") : QStringLiteral("utf8"));
}
#endif
