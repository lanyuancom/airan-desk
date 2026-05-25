#include "terminal_window.h"
#include "common/constant.h"
#include "ui/adaptive_ui.h"
#include "ui/app_title_bar.h"
#include "ui_terminal_window.h"
#include "terminal/terminal_file_panel.h"
#include "terminal/native_terminal_widget.h"
#include "util/config_util.h"
#include "util/json_util.h"
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextStream>
#include <QVBoxLayout>
#include <QUuid>

TerminalWindow::TerminalWindow(QString remoteId, QString remotePwdMd5, WsCli *wsCli, QWidget *parent)
    : QWidget(parent), m_remoteId(remoteId), m_remotePwdMd5(remotePwdMd5),
      m_rtcCtl(remoteId, remotePwdMd5, true), m_ws(wsCli)
{
    initUI();
    initCLI();
    emit initRtcCtl();
}

TerminalWindow::~TerminalWindow()
{
    closeTerminalLogFile();

    if (m_started)
    {
        QJsonObject stopMsg = JsonUtil::createObject()
                                  .add(Constant::KEY_MSGTYPE, Constant::TYPE_TERMINAL_STOP)
                                  .build();
        emit fileTextChannelSendMsg(rtc::message_variant(JsonUtil::toCompactBytes(stopMsg).toStdString()));
    }

    disconnect();
    if (m_rtcThread.isRunning())
    {
        QMetaObject::invokeMethod(&m_rtcCtl, "shutdown", Qt::BlockingQueuedConnection);
    }
    STOP_OBJ_THREAD(m_rtcThread);
    delete ui;
}

void TerminalWindow::initUI()
{
    ui = new Ui::TerminalWindow();
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setWindowTitle(tr("终端：%1").arg(m_remoteId));
    const QSize windowSize = UiAdaptive::applyAdaptiveWindowSize(this, QSize(980, 620), QSize(560, 360));

    auto *titleBar = new AppTitleBar(this, true, true, this);
    ui->titleBarHost->setFixedHeight(titleBar->height());
    ui->titleBarHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->terminalToolsWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->rootLayout->setStretch(0, 0);
    ui->rootLayout->setStretch(1, 0);
    ui->rootLayout->setStretch(2, 1);
    ui->titleBarLayout->addWidget(titleBar);
    m_autoSaveLogCheck = ui->autoSaveLogCheck;
    m_autoSaveLogPathLabel = ui->autoSaveLogPathLabel;
    m_autoSaveLogPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_filePanel = new TerminalFilePanel(ui->terminalSplitter);
    m_terminal = new NativeTerminalWidget(ui->terminalSplitter);
    ui->terminalSplitter->addWidget(m_filePanel);
    ui->terminalSplitter->addWidget(m_terminal);
    ui->terminalSplitter->setStretchFactor(0, 0);
    ui->terminalSplitter->setStretchFactor(1, 1);
    ui->terminalSplitter->setSizes({qMax(220, windowSize.width() / 3), qMax(420, windowSize.width() * 2 / 3)});

    connect(m_terminal, &NativeTerminalWidget::inputGenerated, this, &TerminalWindow::sendTerminalInput);
    connect(m_terminal, &NativeTerminalWidget::gridSizeChanged, this, &TerminalWindow::sendTerminalResize);
    connect(m_terminal, &NativeTerminalWidget::currentDirectoryChanged, m_filePanel, &TerminalFilePanel::followTerminalPath);
    connect(m_filePanel, &TerminalFilePanel::requestFileList, this, &TerminalWindow::requestFileList);
    connect(m_filePanel, &TerminalFilePanel::requestDownload, this, &TerminalWindow::requestDownload);
    connect(m_filePanel, &TerminalFilePanel::requestUpload, this, &TerminalWindow::requestUpload);
    connect(m_terminal, &NativeTerminalWidget::terminalTitleChanged, this, &TerminalWindow::updateTerminalTitle);
    connect(m_autoSaveLogCheck, &QCheckBox::toggled, this, &TerminalWindow::onAutoSaveLogToggled);
    m_terminal->showStatusLine(tr("正在连接远程终端..."));
}

void TerminalWindow::initCLI()
{
    connect(&m_rtcCtl, &WebRtcCtl::sendWsCliBinaryMsg, m_ws, &WsCli::sendWsCliBinaryMsg);
    connect(&m_rtcCtl, &WebRtcCtl::sendWsCliTextMsg, m_ws, &WsCli::sendWsCliTextMsg);
    connect(m_ws, &WsCli::onWsCliRecvBinaryMsg, &m_rtcCtl, &WebRtcCtl::onWsCliRecvBinaryMsg);
    connect(m_ws, &WsCli::onWsCliRecvTextMsg, &m_rtcCtl, &WebRtcCtl::onWsCliRecvTextMsg);

    connect(this, &TerminalWindow::initRtcCtl, &m_rtcCtl, &WebRtcCtl::init);
    connect(this, &TerminalWindow::fileTextChannelSendMsg, &m_rtcCtl, &WebRtcCtl::fileTextChannelSendMsg);
    connect(this, &TerminalWindow::uploadFile2CLI, &m_rtcCtl, &WebRtcCtl::uploadFile2CLI);
    connect(&m_rtcCtl, &WebRtcCtl::fileTextChannelOpened, this, &TerminalWindow::onFileTextChannelOpened);
    connect(&m_rtcCtl, &WebRtcCtl::terminalOutput, this, &TerminalWindow::onTerminalOutput);
    connect(&m_rtcCtl, &WebRtcCtl::terminalInfo, this, &TerminalWindow::onTerminalInfo);
    connect(&m_rtcCtl, &WebRtcCtl::terminalClosed, this, &TerminalWindow::onTerminalClosed);
    connect(&m_rtcCtl, &WebRtcCtl::terminalError, this, &TerminalWindow::onTerminalError);
    connect(&m_rtcCtl, &WebRtcCtl::recvGetFileList, m_filePanel, &TerminalFilePanel::recvGetFileList);
    connect(&m_rtcCtl, &WebRtcCtl::recvDownloadFile, m_filePanel, &TerminalFilePanel::recvDownloadFile);
    connect(&m_rtcCtl, &WebRtcCtl::recvUploadFileRes, m_filePanel, &TerminalFilePanel::recvUploadFile);

    m_rtcThread.setObjectName("TerminalWindow-WebRtcCtlThread");
    m_rtcCtl.moveToThread(&m_rtcThread);
    m_rtcThread.start();
}

void TerminalWindow::updateTerminalTitle(const QString &title)
{
    if (title.isEmpty())
        setWindowTitle(tr("终端：%1").arg(m_remoteId));
    else
        setWindowTitle(tr("%1 - %2").arg(title, m_remoteId));
}

void TerminalWindow::onFileTextChannelOpened()
{
    m_channelReady = true;
    m_filePanel->setConnected(true);
    tryStartTerminal();
}

void TerminalWindow::onTerminalOutput(const QByteArray &data)
{
    if (!m_terminal)
        return;

    const QByteArray output = normalizePipeTerminalOutput(filterTerminalOutput(data));
    appendTerminalLog(output);
    m_terminal->writePtyOutput(output);
}

void TerminalWindow::onTerminalInfo(const QString &osName, const QString &shellPath, const QString &mode, bool pathTracking)
{
    m_remoteOs = osName;
    m_remoteShell = shellPath;
    m_remoteTerminalMode = mode;
    m_remotePathTracking = pathTracking;
    m_lastPipeOutputWasCr = false;
    injectPathTracking();
    if (m_terminal)
        m_terminal->setLocalEchoEnabled(m_remoteTerminalMode == QStringLiteral("pipe"));
}

QByteArray TerminalWindow::filterTerminalOutput(const QByteArray &data)
{
    if (data.isEmpty())
        return data;

    QByteArray output = m_terminalFilterPending + data;
    m_terminalFilterPending.clear();

    const QByteArray markers[] = {
        QByteArrayLiteral("prompt $E]7;file:"),
        QByteArrayLiteral("$E]7;file:///$P$E\\$G$S$P$G"),
        QByteArrayLiteral("function global:prompt"),
        QByteArrayLiteral("export AIRAN_OLD_PROMPT_COMMAND="),
        QByteArrayLiteral("export PROMPT_COMMAND="),
        QByteArrayLiteral("file://localhost%s")};

    for (;;)
    {
        int marker = -1;
        for (const QByteArray &candidate : markers)
        {
            const int index = output.indexOf(candidate);
            if (index >= 0 && (marker < 0 || index < marker))
                marker = index;
        }
        if (marker < 0)
            break;

        const int lastCr = output.lastIndexOf('\r', marker);
        const int lastLf = output.lastIndexOf('\n', marker);
        const int lineStart = qMax(lastCr, lastLf) + 1;
        int nextCr = output.indexOf('\r', marker);
        int nextLf = output.indexOf('\n', marker);
        if (nextCr < 0)
            nextCr = nextLf;
        else if (nextLf >= 0)
            nextCr = qMin(nextCr, nextLf);

        if (nextCr < 0)
        {
            m_terminalFilterPending = output.mid(lineStart);
            output.truncate(lineStart);
            break;
        }

        int removeEnd = nextCr + 1;
        while (removeEnd < output.size() && (output.at(removeEnd) == '\r' || output.at(removeEnd) == '\n'))
            ++removeEnd;
        output.remove(lineStart, removeEnd - lineStart);
        m_filterWindowsPromptEcho = false;
    }

    if (m_filterWindowsPromptEcho)
    {
        const int keepBytes = qMin(output.size(), 96);
        const QByteArray tail = output.right(keepBytes);
        if (tail.contains("prompt ") || tail.contains("$E]7;") || tail.contains("function global"))
        {
            m_terminalFilterPending = tail;
            output.chop(keepBytes);
        }
    }

    return output;
}

QByteArray TerminalWindow::normalizePipeTerminalOutput(const QByteArray &data)
{
    if (data.isEmpty() || m_remoteTerminalMode != QStringLiteral("pipe"))
        return data;

    QByteArray output;
    output.reserve(data.size() + 16);
    for (char ch : data)
    {
        if (ch == '\n' && !m_lastPipeOutputWasCr)
            output.append('\r');

        output.append(ch);
        if (ch == '\r')
            m_lastPipeOutputWasCr = true;
        else if (ch == '\n')
            m_lastPipeOutputWasCr = false;
        else
            m_lastPipeOutputWasCr = false;
    }
    return output;
}

void TerminalWindow::onAutoSaveLogToggled(bool checked)
{
    setTerminalAutoSave(checked);
}

void TerminalWindow::setTerminalAutoSave(bool enabled)
{
    if (enabled)
    {
        if (!openTerminalLogFile())
        {
            if (m_autoSaveLogCheck)
            {
                m_autoSaveLogCheck->blockSignals(true);
                m_autoSaveLogCheck->setChecked(false);
                m_autoSaveLogCheck->blockSignals(false);
            }
            m_terminalLogEnabled = false;
            updateTerminalLogUi();
            if (ConfigUtil->showUI)
                QMessageBox::warning(this, tr("终端记录"), tr("无法创建终端记录文件"));
            return;
        }
        m_terminalLogEnabled = true;
    }
    else
    {
        closeTerminalLogFile();
        m_terminalLogEnabled = false;
    }
    updateTerminalLogUi();
}

bool TerminalWindow::openTerminalLogFile()
{
    if (m_terminalLogFile.isOpen())
        return true;

    m_terminalLogPath = defaultTerminalLogPath();
    QFileInfo fileInfo(m_terminalLogPath);
    QDir dir(fileInfo.absolutePath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
        return false;

    m_terminalLogFile.setFileName(m_terminalLogPath);
    if (!m_terminalLogFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;

    QTextStream stream(&m_terminalLogFile);
    stream.setCodec("UTF-8");
    stream << "# Airan Desk Terminal Log\n";
    stream << "# Remote ID: " << m_remoteId << "\n";
    stream << "# Started At: " << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << "\n";
    stream << "# Encoding: UTF-8\n";
    stream << "# Format: [local-time] output-line\n\n";
    stream.flush();
    return true;
}

void TerminalWindow::closeTerminalLogFile()
{
    if (!m_terminalLogPendingLine.isEmpty())
    {
        writeTerminalLogLine(m_terminalLogPendingLine);
        m_terminalLogPendingLine.clear();
    }

    if (m_terminalLogFile.isOpen())
    {
        QTextStream stream(&m_terminalLogFile);
        stream.setCodec("UTF-8");
        stream << "\n# Closed At: " << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << "\n";
        stream.flush();
        m_terminalLogFile.close();
    }
}

void TerminalWindow::appendTerminalLog(const QByteArray &data)
{
    if (!m_terminalLogEnabled || !m_terminalLogFile.isOpen() || data.isEmpty())
        return;

    const QString text = plainTextFromTerminalOutput(data);
    for (int i = 0; i < text.size(); ++i)
    {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('\n'))
        {
            writeTerminalLogLine(m_terminalLogPendingLine);
            m_terminalLogPendingLine.clear();
        }
        else
        {
            m_terminalLogPendingLine.append(ch);
        }
    }
}

QString TerminalWindow::plainTextFromTerminalOutput(const QByteArray &data) const
{
    enum ParserState
    {
        NormalState,
        EscapeState,
        CsiState,
        OscState,
        OscEscapeState
    };

    ParserState state = NormalState;
    QString output;
    const QString text = QString::fromUtf8(data);
    for (int i = 0; i < text.size(); ++i)
    {
        const QChar ch = text.at(i);
        const ushort code = ch.unicode();
        if (state == NormalState)
        {
            if (code == 0x1b)
            {
                state = EscapeState;
            }
            else if (ch == QLatin1Char('\b'))
            {
                if (!output.isEmpty() && output.at(output.size() - 1) != QLatin1Char('\n'))
                    output.chop(1);
            }
            else if (ch == QLatin1Char('\r'))
            {
            }
            else if (ch == QLatin1Char('\n') || ch == QLatin1Char('\t') || code >= 0x20)
            {
                output.append(ch);
            }
        }
        else if (state == EscapeState)
        {
            if (ch == QLatin1Char('['))
                state = CsiState;
            else if (ch == QLatin1Char(']'))
                state = OscState;
            else
                state = NormalState;
        }
        else if (state == CsiState)
        {
            if (code >= 0x40 && code <= 0x7e)
                state = NormalState;
        }
        else if (state == OscState)
        {
            if (code == 0x07)
                state = NormalState;
            else if (code == 0x1b)
                state = OscEscapeState;
        }
        else if (state == OscEscapeState)
        {
            state = NormalState;
        }
    }
    return output;
}

void TerminalWindow::writeTerminalLogLine(const QString &line)
{
    if (!m_terminalLogFile.isOpen())
        return;

    QTextStream stream(&m_terminalLogFile);
    stream.setCodec("UTF-8");
    stream << "[" << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) << "] "
           << line << "\n";
    stream.flush();
}

QString TerminalWindow::defaultTerminalLogPath() const
{
    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (baseDir.isEmpty())
        baseDir = QDir::homePath();

    QString safeRemoteId = m_remoteId;
    const QChar replacement = QLatin1Char('_');
    safeRemoteId.replace(QLatin1Char('/'), replacement);
    safeRemoteId.replace(QLatin1Char('\\'), replacement);
    safeRemoteId.replace(QLatin1Char(':'), replacement);
    safeRemoteId.replace(QLatin1Char('*'), replacement);
    safeRemoteId.replace(QLatin1Char('?'), replacement);
    safeRemoteId.replace(QLatin1Char('"'), replacement);
    safeRemoteId.replace(QLatin1Char('<'), replacement);
    safeRemoteId.replace(QLatin1Char('>'), replacement);
    safeRemoteId.replace(QLatin1Char('|'), replacement);
    if (safeRemoteId.isEmpty())
        safeRemoteId = QStringLiteral("remote");

    const QString fileName = QStringLiteral("terminal_%1_%2.txt")
                                 .arg(safeRemoteId,
                                      QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    return QDir(baseDir).filePath(QStringLiteral("AiranDesk/terminal_logs/") + fileName);
}

void TerminalWindow::updateTerminalLogUi()
{
    if (!m_autoSaveLogPathLabel)
        return;

    if (m_terminalLogEnabled && !m_terminalLogPath.isEmpty())
        m_autoSaveLogPathLabel->setText(tr("记录到: %1").arg(QDir::toNativeSeparators(m_terminalLogPath)));
    else
        m_autoSaveLogPathLabel->setText(tr("未启用"));
}

void TerminalWindow::onTerminalClosed(int exitCode)
{
    m_terminal->showStatusLine(tr("[远程终端已退出，退出码 %1]").arg(exitCode));
}

void TerminalWindow::onTerminalError(const QString &message)
{
    m_terminal->showStatusLine(tr("[终端错误] %1").arg(message));
    if (ConfigUtil->showUI)
        QMessageBox::warning(this, tr("终端错误"), message);
}

void TerminalWindow::tryStartTerminal()
{
    if (m_started || !m_channelReady || !m_terminal)
        return;

    m_started = true;
    const QSize grid = m_terminal->gridSize();
    QJsonObject msg = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_TERMINAL_START)
                          .add(Constant::KEY_COLS, grid.width())
                          .add(Constant::KEY_ROWS, grid.height())
                          .build();
    emit fileTextChannelSendMsg(rtc::message_variant(JsonUtil::toCompactBytes(msg).toStdString()));
}

void TerminalWindow::sendTerminalResize(const QSize &gridSize)
{
    if (!m_started)
        return;

    QJsonObject msg = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_TERMINAL_RESIZE)
                          .add(Constant::KEY_COLS, gridSize.width())
                          .add(Constant::KEY_ROWS, gridSize.height())
                          .build();
    emit fileTextChannelSendMsg(rtc::message_variant(JsonUtil::toCompactBytes(msg).toStdString()));
}

void TerminalWindow::sendTerminalInput(const QByteArray &data)
{
    if (!m_started || data.isEmpty())
        return;

    QJsonObject msg = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_TERMINAL_INPUT)
                          .add(Constant::KEY_ENCODING, QStringLiteral("base64"))
                          .add(Constant::KEY_DATA, QString::fromLatin1(data.toBase64()))
                          .build();
    emit fileTextChannelSendMsg(rtc::message_variant(JsonUtil::toCompactBytes(msg).toStdString()));
}

void TerminalWindow::requestFileList(const QString &path)
{
    if (!m_started || path.isEmpty())
        return;

    QJsonObject msg = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_LIST)
                          .add(Constant::KEY_PATH, path)
                          .build();
    emit fileTextChannelSendMsg(rtc::message_variant(JsonUtil::toCompactBytes(msg).toStdString()));
}

void TerminalWindow::requestDownload(const QString &remotePath, const QString &localPath, bool isDirectory)
{
    if (!m_started || remotePath.isEmpty() || localPath.isEmpty())
        return;

    QJsonObject msg = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_DOWNLOAD)
                          .add(Constant::KEY_PATH_CLI, remotePath)
                          .add(Constant::KEY_PATH_CTL, localPath)
                          .add("isDirectory", isDirectory)
                          .build();
    emit fileTextChannelSendMsg(rtc::message_variant(JsonUtil::toCompactBytes(msg).toStdString()));
}

void TerminalWindow::requestUpload(const QString &localPath, const QString &remotePath, bool)
{
    if (!m_started || localPath.isEmpty() || remotePath.isEmpty())
        return;

    {
        QString uuid = QUuid::createUuid().toString();
        if (uuid.startsWith(QLatin1Char('{')) && uuid.endsWith(QLatin1Char('}')))
            uuid = uuid.mid(1, uuid.size() - 2);
        emit uploadFile2CLI(localPath, remotePath, uuid);
    }
}

void TerminalWindow::injectPathTracking()
{
    if (!m_started || !m_remotePathTracking)
        return;

    QByteArray script;
    if (m_remoteOs == QStringLiteral("windows"))
    {
        const QString shell = m_remoteShell.toLower();
        if (shell.contains(QStringLiteral("powershell")) || shell.contains(QStringLiteral("pwsh")))
        {
            script = "function global:prompt { $p=(Get-Location).Path; $u='file:///'+$p.Replace('\\\\','/').Replace(' ','%20'); [Console]::Write(\"`e]7;$u`a\"); \"PS $p> \" }\r";
        }
        else
        {
            m_filterWindowsPromptEcho = true;
            script = "prompt $E]7;file:///$P$E\\$G$S$P$G\r";
        }
    }
    else
    {
        script = "export AIRAN_OLD_PROMPT_COMMAND=\"$PROMPT_COMMAND\"\n"
                 "export PROMPT_COMMAND='printf \"\\033]7;file://localhost%s\\007\" \"$PWD\"; if [ -n \"$AIRAN_OLD_PROMPT_COMMAND\" ]; then eval \"$AIRAN_OLD_PROMPT_COMMAND\"; fi'\n";
    }
    sendTerminalInput(script);
}
