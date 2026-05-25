#include "terminal_session.h"

#if !defined(Q_OS_WIN)

#include <QThread>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <util.h>
#else
#include <pty.h>
#endif

bool TerminalSession::startPty(int cols, int rows)
{
    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);

    pid_t pid = forkpty(&m_masterFd, nullptr, nullptr, &ws);
    if (pid < 0)
    {
        emit errorOccurred(QStringLiteral("Failed to create pseudo terminal: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    if (pid == 0)
    {
        setenv("TERM", "xterm-256color", 1);
        QByteArray shell = defaultShell().toLocal8Bit();
        execlp(shell.constData(), shell.constData(), static_cast<char *>(nullptr));
        _exit(127);
    }

    m_childPid = pid;
    int flags = fcntl(m_masterFd, F_GETFL, 0);
    fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);

    m_notifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &TerminalSession::onPtyReadyRead);

    return true;
}

void TerminalSession::onPtyReadyRead()
{
    QByteArray buffer(8192, Qt::Uninitialized);
    for (;;)
    {
        ssize_t n = ::read(m_masterFd, buffer.data(), static_cast<size_t>(buffer.size()));
        if (n > 0)
        {
            emit outputReady(buffer.left(static_cast<int>(n)));
            continue;
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
        {
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

            int status = 0;
            int exitCode = 0;
            if (m_childPid > 0 && waitpid(static_cast<pid_t>(m_childPid), &status, WNOHANG) > 0)
            {
                if (WIFEXITED(status))
                    exitCode = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    exitCode = 128 + WTERMSIG(status);
                m_childPid = -1;
            }
            emitClosedOnce(exitCode);
        }
        break;
    }
}

void TerminalSession::reapChildProcessNonBlocking()
{
    if (m_childPid <= 0)
        return;

    for (int i = 0; i < 10; ++i)
    {
        int status = 0;
        const pid_t result = ::waitpid(static_cast<pid_t>(m_childPid), &status, WNOHANG);
        if (result == static_cast<pid_t>(m_childPid) || result < 0)
        {
            m_childPid = -1;
            return;
        }
        QThread::msleep(20);
    }
}

#endif
