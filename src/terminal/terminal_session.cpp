#include "terminal_session.h"
#include "common/logger_manager.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QThread>
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

#if !defined(Q_OS_WIN)
#include <cerrno>
#include <cstring>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

TerminalSession::TerminalSession(QObject *parent)
    : QObject(parent)
{
#if defined(Q_OS_WIN)
    m_windowsInputFlushTimer = new QTimer(this);
    m_windowsInputFlushTimer->setSingleShot(true);
    connect(m_windowsInputFlushTimer, &QTimer::timeout, this, &TerminalSession::flushWindowsPendingInput);
#endif
}

TerminalSession::~TerminalSession()
{
    stop();
}

QString TerminalSession::defaultShell() const
{
#if defined(Q_OS_WIN)
    const QString comspec = QString::fromLocal8Bit(qgetenv("ComSpec"));
    return comspec.isEmpty() ? QStringLiteral("C:/Windows/System32/cmd.exe") : QDir::fromNativeSeparators(comspec);
#else
    const QString shell = QString::fromLocal8Bit(qgetenv("SHELL"));
#if defined(Q_OS_MACOS)
    return shell.isEmpty() ? QStringLiteral("/bin/zsh") : shell;
#else
    return shell.isEmpty() ? QStringLiteral("/bin/bash") : shell;
#endif
#endif
}

void TerminalSession::start(int cols, int rows)
{
    stop();
    m_closedEmitted.store(false);
#if defined(Q_OS_WIN)
    m_windowsConsoleDecodePending.clear();
    m_windowsUtf8DecodePending.clear();
    m_windowsOutputLogCount = 0;
#endif
    const int safeCols = qMax(20, cols);
    const int safeRows = qMax(5, rows);
#if defined(Q_OS_WIN)
    if (startPty(safeCols, safeRows))
    {
        LOG_INFO("Terminal started with Windows ConPTY mode");
        emitTerminalInfo(QStringLiteral("pty"), true);
        return;
    }

    LOG_WARN("Windows ConPTY is not available; starting plain shell fallback");
    if (startFallbackProcess())
    {
        LOG_INFO("Terminal started with Windows pipe fallback mode");
        emitTerminalInfo(QStringLiteral("pipe"), true);
        emit outputReady(QStringLiteral("\r\n[Windows 7 compatibility terminal]\r\n").toUtf8());
    }
#else
    if (startPty(safeCols, safeRows))
    {
        emitTerminalInfo(QStringLiteral("pty"), true);
    }
    else
    {
        emit errorOccurred(QStringLiteral("Pseudo terminal is not available on this system"));
    }
#endif
}

void TerminalSession::emitTerminalInfo(const QString &mode, bool pathTracking)
{
    QString osName;
#if defined(Q_OS_WIN)
    osName = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    osName = QStringLiteral("macos");
#else
    osName = QStringLiteral("linux");
#endif
    emit terminalInfoReady(osName, defaultShell(), mode, pathTracking);
}

void TerminalSession::emitClosedOnce(int exitCode)
{
    if (m_closedEmitted.exchange(true))
        return;
    emit closed(exitCode);
}

bool TerminalSession::startFallbackProcess()
{
    m_usingFallbackProcess = true;
#if defined(Q_OS_WIN)
    return startWindowsFallbackProcess();
#else
    m_process = new QProcess(this);
    m_process->setProgram(defaultShell());
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setInputChannelMode(QProcess::ManagedInputChannel);

    connect(m_process, &QProcess::readyRead, this, &TerminalSession::onFallbackReadyRead);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &TerminalSession::onFallbackFinished);
    connect(m_process, QOverload<QProcess::ProcessError>::of(&QProcess::errorOccurred),
            this, &TerminalSession::onFallbackError);

    m_process->start();
    if (!m_process->waitForStarted(3000))
    {
        emit errorOccurred(m_process->errorString());
        return false;
    }
    return true;
#endif
}

void TerminalSession::onFallbackReadyRead()
{
    if (m_process)
    {
#if defined(Q_OS_WIN)
        const QByteArray data = m_process->readAll();
        logWindowsTerminalBytes(data, QStringLiteral("pipe"));
        emit outputReady(decodeWindowsOutput(data.constData(), data.size()));
#else
        emit outputReady(m_process->readAll());
#endif
    }
}

void TerminalSession::onFallbackFinished(int exitCode, QProcess::ExitStatus)
{
    emitClosedOnce(exitCode);
}

void TerminalSession::onFallbackError(QProcess::ProcessError)
{
    emit errorOccurred(m_process ? m_process->errorString() : QStringLiteral("Terminal process error"));
}

void TerminalSession::writeInput(const QByteArray &data)
{
    if (data.isEmpty())
        return;

    LOG_INFO("Terminal write input requested: size={}, text={}", data.size(), terminalSessionPreview(data));

#if defined(Q_OS_WIN)
    if (m_usingFallbackProcess && m_inputWrite)
    {
        const QByteArray normalized = normalizeFallbackInput(data);
        DWORD written = 0;
        WriteFile(m_inputWrite, normalized.constData(), static_cast<DWORD>(normalized.size()), &written, nullptr);
        LOG_TRACE("Terminal Windows fallback input size={}, written={}",
                  normalized.size(),
                  written);
        return;
    }
#else
    if (m_usingFallbackProcess && m_process)
    {
        m_process->write(data);
        return;
    }
#endif

#if defined(Q_OS_WIN)
    if (m_inputWrite)
    {
        const QByteArray localInput = normalizeWindowsInput(data);
        if (localInput.isEmpty())
            return;

        m_windowsPendingInput.append(localInput);
        if (windowsInputNeedsImmediateFlush(localInput))
        {
            flushWindowsPendingInput();
        }
        else
        {
            if (m_windowsInputFlushTimer)
                m_windowsInputFlushTimer->start(120);
            LOG_TRACE("Terminal ConPTY input queued: pendingSize={}, text={}, hex={}",
                      m_windowsPendingInput.size(),
                      terminalSessionPreview(m_windowsPendingInput),
                      terminalSessionHexPreview(m_windowsPendingInput));
        }
    }
#else
    if (m_masterFd >= 0)
    {
        const ssize_t written = ::write(m_masterFd, data.constData(), static_cast<size_t>(data.size()));
        if (written < 0)
            LOG_WARN("Failed to write terminal input to pty: {}", QString::fromLocal8Bit(strerror(errno)));
    }
#endif
}

void TerminalSession::resize(int cols, int rows)
{
    cols = qMax(20, cols);
    rows = qMax(5, rows);

    if (m_usingFallbackProcess)
        return;

#if defined(Q_OS_WIN)
    typedef HRESULT(WINAPI * ResizePseudoConsoleFn)(HPCON, COORD);
    static ResizePseudoConsoleFn resizePseudoConsole =
        reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ResizePseudoConsole"));
    if (resizePseudoConsole && m_hpc)
    {
        COORD size{static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
        resizePseudoConsole(m_hpc, size);
    }
#else
    if (m_masterFd >= 0)
    {
        struct winsize ws{};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        ioctl(m_masterFd, TIOCSWINSZ, &ws);
    }
#endif
}

void TerminalSession::stop()
{
    m_closedEmitted.store(true);
    if (m_process)
    {
        m_process->terminate();
        if (!m_process->waitForFinished(1000))
            m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_usingFallbackProcess = false;

#if defined(Q_OS_WIN)
    if (m_windowsInputFlushTimer)
        m_windowsInputFlushTimer->stop();
    m_windowsPendingInput.clear();
    closeConPty();
#else
    if (m_notifier)
    {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_masterFd >= 0)
    {
        ::close(m_masterFd);
        m_masterFd = -1;
    }
    if (m_childPid > 0)
    {
        ::kill(static_cast<pid_t>(m_childPid), SIGHUP);
        reapChildProcessNonBlocking();
    }
#endif
}
