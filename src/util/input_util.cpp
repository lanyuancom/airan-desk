#include "input_util.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include "../common/logger_manager.h"

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

InputUtil::InputUtil(QObject *parent)
    : QObject{parent}
{
}

bool InputUtil::runProgram(const QString &path, QString *errorMessage)
{
    QFileInfo info(path);
    if (!info.exists() || !info.isFile())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("文件不存在或不是普通文件");
        LOG_WARN("runProgram rejected invalid path: {}", path);
        return false;
    }

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    const QString nativePath = QDir::toNativeSeparators(info.absoluteFilePath());
    const QString nativeDir = QDir::toNativeSeparators(info.absolutePath());
    HINSTANCE result = ShellExecuteW(nullptr,
                                     L"open",
                                     reinterpret_cast<LPCWSTR>(nativePath.utf16()),
                                     nullptr,
                                     reinterpret_cast<LPCWSTR>(nativeDir.utf16()),
                                     SW_SHOWNORMAL);
    const bool ok = reinterpret_cast<INT_PTR>(result) > 32;
    if (!ok && errorMessage)
        *errorMessage = QStringLiteral("ShellExecute 执行失败");
    LOG_INFO("runProgram {} path={}", ok ? "succeeded" : "failed", info.absoluteFilePath());
    return ok;
#else
    const bool ok = QProcess::startDetached(info.absoluteFilePath(), QStringList(), info.absolutePath());
    if (!ok && errorMessage)
        *errorMessage = QStringLiteral("启动程序失败");
    LOG_INFO("runProgram {} path={}", ok ? "succeeded" : "failed", info.absoluteFilePath());
    return ok;
#endif
}

bool InputUtil::execRemoteOperation(const QString &action, QString *errorMessage)
{
    const QString normalized = action.trimmed().toLower();

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    if (normalized == QStringLiteral("lock"))
        return LockWorkStation() != FALSE;
    if (normalized == QStringLiteral("logoff"))
        return ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_APPLICATION) != FALSE;
    if (normalized == QStringLiteral("restart"))
        return QProcess::startDetached(QStringLiteral("shutdown"), QStringList() << QStringLiteral("/r") << QStringLiteral("/t") << QStringLiteral("0"));
    if (normalized == QStringLiteral("shutdown"))
        return QProcess::startDetached(QStringLiteral("shutdown"), QStringList() << QStringLiteral("/s") << QStringLiteral("/t") << QStringLiteral("0"));
    if (normalized == QStringLiteral("resource_manager"))
        return QProcess::startDetached(QStringLiteral("explorer.exe"), QStringList());
    if (normalized == QStringLiteral("task_manager"))
        return QProcess::startDetached(QStringLiteral("taskmgr.exe"), QStringList());
#elif defined(Q_OS_MACOS)
    if (normalized == QStringLiteral("lock"))
        return QProcess::startDetached(QStringLiteral("/System/Library/CoreServices/Menu Extras/User.menu/Contents/Resources/CGSession"), QStringList() << QStringLiteral("-suspend"));
    if (normalized == QStringLiteral("restart"))
        return QProcess::startDetached(QStringLiteral("osascript"), QStringList() << QStringLiteral("-e") << QStringLiteral("tell app \"System Events\" to restart"));
    if (normalized == QStringLiteral("shutdown"))
        return QProcess::startDetached(QStringLiteral("osascript"), QStringList() << QStringLiteral("-e") << QStringLiteral("tell app \"System Events\" to shut down"));
    if (normalized == QStringLiteral("resource_manager"))
        return QProcess::startDetached(QStringLiteral("open"), QStringList() << QStringLiteral("-a") << QStringLiteral("Finder"));
    if (normalized == QStringLiteral("task_manager"))
        return QProcess::startDetached(QStringLiteral("open"), QStringList() << QStringLiteral("-a") << QStringLiteral("Activity Monitor"));
#else
    if (normalized == QStringLiteral("lock"))
        return QProcess::startDetached(QStringLiteral("loginctl"), QStringList() << QStringLiteral("lock-session"));
    if (normalized == QStringLiteral("logoff"))
        return QProcess::startDetached(QStringLiteral("loginctl"), QStringList() << QStringLiteral("terminate-user") << qgetenv("USER"));
    if (normalized == QStringLiteral("restart"))
        return QProcess::startDetached(QStringLiteral("systemctl"), QStringList() << QStringLiteral("reboot"));
    if (normalized == QStringLiteral("shutdown"))
        return QProcess::startDetached(QStringLiteral("systemctl"), QStringList() << QStringLiteral("poweroff"));
    if (normalized == QStringLiteral("resource_manager"))
        return QProcess::startDetached(QStringLiteral("xdg-open"), QStringList() << QDir::homePath());
    if (normalized == QStringLiteral("task_manager"))
        return QProcess::startDetached(QStringLiteral("xterm"), QStringList() << QStringLiteral("-e") << QStringLiteral("top"));
#endif

    if (errorMessage)
        *errorMessage = QStringLiteral("不支持的远程操作");
    LOG_WARN("Unsupported remote operation: {}", action);
    return false;
}

bool InputUtil::execAndroidNavigation(const QString &action, QString *errorMessage)
{
    const QString normalized = action.trimmed().toLower();
    QString keyCode;
    if (normalized == QStringLiteral("back"))
        keyCode = QStringLiteral("KEYCODE_BACK");
    else if (normalized == QStringLiteral("home"))
        keyCode = QStringLiteral("KEYCODE_HOME");
    else if (normalized == QStringLiteral("menu"))
        keyCode = QStringLiteral("KEYCODE_MENU");
    else if (normalized == QStringLiteral("recents"))
        keyCode = QStringLiteral("KEYCODE_APP_SWITCH");

    if (keyCode.isEmpty())
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("不支持的安卓导航动作");
        LOG_WARN("Unsupported Android navigation action: {}", action);
        return false;
    }

#if defined(Q_OS_ANDROID)
    const bool ok = QProcess::execute(QStringLiteral("input"), QStringList() << QStringLiteral("keyevent") << keyCode) == 0;
    if (!ok && errorMessage)
        *errorMessage = QStringLiteral("Android input keyevent 执行失败");
    LOG_INFO("Android navigation {} action={}", ok ? "succeeded" : "failed", normalized);
    return ok;
#else
    if (errorMessage)
        *errorMessage = QStringLiteral("当前被控端不是 Android");
    LOG_WARN("Android navigation ignored on non-Android platform: {}", action);
    return false;
#endif
}
