/* Platform input event implementation split from input_util_events.cpp. */

#include "input_util.h"

#include <QGuiApplication>
#include <QProcess>
#include <QRect>
#include <QScreen>
#include <QStandardPaths>
#include <algorithm>
#include <chrono>
#include <thread>

#include "../common/logger_manager.h"

#if defined(Q_OS_LINUX)
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

namespace
{

    static int s_keyboardFd = -1; /* uinput 虚拟键盘 fd，进程生命周期内保持打开 */
    static int s_mouseFd = -1;    /* uinput 虚拟鼠标 fd，进程生命周期内保持打开 */
    static int s_screenW = 1920;
    static int s_screenH = 1080;

    /* Windows VK 码 → Linux input keycode */
    static int vkToLinux(int vk)
    {
        switch (vk)
        {
        case 0x08:
            return KEY_BACKSPACE;
        case 0x09:
            return KEY_TAB;
        case 0x0D:
            return KEY_ENTER;
        case 0x10:
            return KEY_LEFTSHIFT;
        case 0x11:
            return KEY_LEFTCTRL;
        case 0x12:
            return KEY_LEFTALT;
        case 0x13:
            return KEY_PAUSE;
        case 0x14:
            return KEY_CAPSLOCK;
        case 0x1B:
            return KEY_ESC;
        case 0x20:
            return KEY_SPACE;
        case 0x21:
            return KEY_PAGEUP;
        case 0x22:
            return KEY_PAGEDOWN;
        case 0x23:
            return KEY_END;
        case 0x24:
            return KEY_HOME;
        case 0x25:
            return KEY_LEFT;
        case 0x26:
            return KEY_UP;
        case 0x27:
            return KEY_RIGHT;
        case 0x28:
            return KEY_DOWN;
        case 0x2A:
            return KEY_SYSRQ; /* VK_PRINT */
        case 0x2C:
            return KEY_SYSRQ; /* VK_SNAPSHOT */
        case 0x2D:
            return KEY_INSERT;
        case 0x2E:
            return KEY_DELETE;
        case 0x30:
            return KEY_0;
        case 0x31:
            return KEY_1;
        case 0x32:
            return KEY_2;
        case 0x33:
            return KEY_3;
        case 0x34:
            return KEY_4;
        case 0x35:
            return KEY_5;
        case 0x36:
            return KEY_6;
        case 0x37:
            return KEY_7;
        case 0x38:
            return KEY_8;
        case 0x39:
            return KEY_9;
        case 0x41:
            return KEY_A;
        case 0x42:
            return KEY_B;
        case 0x43:
            return KEY_C;
        case 0x44:
            return KEY_D;
        case 0x45:
            return KEY_E;
        case 0x46:
            return KEY_F;
        case 0x47:
            return KEY_G;
        case 0x48:
            return KEY_H;
        case 0x49:
            return KEY_I;
        case 0x4A:
            return KEY_J;
        case 0x4B:
            return KEY_K;
        case 0x4C:
            return KEY_L;
        case 0x4D:
            return KEY_M;
        case 0x4E:
            return KEY_N;
        case 0x4F:
            return KEY_O;
        case 0x50:
            return KEY_P;
        case 0x51:
            return KEY_Q;
        case 0x52:
            return KEY_R;
        case 0x53:
            return KEY_S;
        case 0x54:
            return KEY_T;
        case 0x55:
            return KEY_U;
        case 0x56:
            return KEY_V;
        case 0x57:
            return KEY_W;
        case 0x58:
            return KEY_X;
        case 0x59:
            return KEY_Y;
        case 0x5A:
            return KEY_Z;
        case 0x5D:
            return KEY_COMPOSE; /* VK_APPS / Menu */
        case 0x6A:
            return KEY_KPASTERISK; /* VK_MULTIPLY */
        case 0x70:
            return KEY_F1;
        case 0x71:
            return KEY_F2;
        case 0x72:
            return KEY_F3;
        case 0x73:
            return KEY_F4;
        case 0x74:
            return KEY_F5;
        case 0x75:
            return KEY_F6;
        case 0x76:
            return KEY_F7;
        case 0x77:
            return KEY_F8;
        case 0x78:
            return KEY_F9;
        case 0x79:
            return KEY_F10;
        case 0x7A:
            return KEY_F11;
        case 0x7B:
            return KEY_F12;
        case 0x7C:
            return KEY_F13;
        case 0x7D:
            return KEY_F14;
        case 0x7E:
            return KEY_F15;
        case 0x7F:
            return KEY_F16;
        case 0x80:
            return KEY_F17;
        case 0x81:
            return KEY_F18;
        case 0x82:
            return KEY_F19;
        case 0x83:
            return KEY_F20;
        case 0x84:
            return KEY_F21;
        case 0x85:
            return KEY_F22;
        case 0x86:
            return KEY_F23;
        case 0x87:
            return KEY_F24;
        case 0x90:
            return KEY_NUMLOCK;
        case 0x91:
            return KEY_SCROLLLOCK;
        case 0xAD:
            return KEY_MUTE;
        case 0xAE:
            return KEY_VOLUMEDOWN;
        case 0xAF:
            return KEY_VOLUMEUP;
        case 0xB2:
            return KEY_STOPCD;
        case 0xB3:
            return KEY_PLAYPAUSE;
        case 0xBA:
            return KEY_SEMICOLON;
        case 0xBB:
            return KEY_EQUAL;
        case 0xBC:
            return KEY_COMMA;
        case 0xBD:
            return KEY_MINUS;
        case 0xBE:
            return KEY_DOT;
        case 0xBF:
            return KEY_SLASH;
        case 0xC0:
            return KEY_GRAVE;
        case 0xDB:
            return KEY_LEFTBRACE;
        case 0xDC:
            return KEY_BACKSLASH;
        case 0xDD:
            return KEY_RIGHTBRACE;
        case 0xDE:
            return KEY_APOSTROPHE;
        default:
            return -1;
        }
    }

    /* 写入一条 input_event */
    static bool uiWrite(int fd, uint16_t type, uint16_t code, int32_t val)
    {
        struct input_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = type;
        ev.code = code;
        ev.value = val;

        ssize_t n = ::write(fd, &ev, sizeof(ev));
        if (n != static_cast<ssize_t>(sizeof(ev)))
        {
            LOG_WARN("Failed to write uinput event: fd={}, type={}, code={}, value={}, written={}, errno={}",
                     fd, type, code, val, n, errno);
            return false;
        }
        return true;
    }

    static void uiSync(int fd)
    {
        uiWrite(fd, EV_SYN, SYN_REPORT, 0);
    }

    static void tapKey(int fd, int key)
    {
        uiWrite(fd, EV_KEY, static_cast<uint16_t>(key), 1);
        uiSync(fd);
        uiWrite(fd, EV_KEY, static_cast<uint16_t>(key), 0);
        uiSync(fd);
    }

    static void pasteShortcut(int fd)
    {
        uiWrite(fd, EV_KEY, KEY_LEFTCTRL, 1);
        uiSync(fd);
        tapKey(fd, KEY_V);
        uiWrite(fd, EV_KEY, KEY_LEFTCTRL, 0);
        uiSync(fd);
    }

    static bool runClipboardWriter(const QString &program, const QStringList &args, const QByteArray &bytes)
    {
        QProcess process;
        process.start(program, args);
        if (!process.waitForStarted(1000))
            return false;
        process.write(bytes);
        process.closeWriteChannel();
        if (!process.waitForFinished(2000))
        {
            process.kill();
            process.waitForFinished(500);
            return false;
        }
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    }

    static bool runClipboardReader(const QString &program, const QStringList &args, QByteArray *out)
    {
        QProcess process;
        process.start(program, args);
        if (!process.waitForStarted(1000))
            return false;
        if (!process.waitForFinished(2000))
        {
            process.kill();
            process.waitForFinished(500);
            return false;
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            return false;
        if (out)
            *out = process.readAllStandardOutput();
        return true;
    }

    struct ClipboardTool
    {
        QString program;
        QStringList readArgs;
        QStringList writeArgs;
    };

    static ClipboardTool findClipboardTool()
    {
        const bool wayland = !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
        if (wayland)
        {
            const QString wlCopy = QStandardPaths::findExecutable(QStringLiteral("wl-copy"));
            const QString wlPaste = QStandardPaths::findExecutable(QStringLiteral("wl-paste"));
            if (!wlCopy.isEmpty() && !wlPaste.isEmpty())
                return ClipboardTool{wlCopy, QStringList{QStringLiteral("--no-newline")}, QStringList()};
        }

        const QString xclip = QStandardPaths::findExecutable(QStringLiteral("xclip"));
        if (!xclip.isEmpty())
            return ClipboardTool{xclip,
                                 QStringList{QStringLiteral("-selection"), QStringLiteral("clipboard"), QStringLiteral("-o")},
                                 QStringList{QStringLiteral("-selection"), QStringLiteral("clipboard")}};

        const QString xsel = QStandardPaths::findExecutable(QStringLiteral("xsel"));
        if (!xsel.isEmpty())
            return ClipboardTool{xsel,
                                 QStringList{QStringLiteral("--clipboard"), QStringLiteral("--output")},
                                 QStringList{QStringLiteral("--clipboard"), QStringLiteral("--input")}};

        if (!wayland)
        {
            const QString wlCopy = QStandardPaths::findExecutable(QStringLiteral("wl-copy"));
            const QString wlPaste = QStandardPaths::findExecutable(QStringLiteral("wl-paste"));
            if (!wlCopy.isEmpty() && !wlPaste.isEmpty())
                return ClipboardTool{wlCopy, QStringList{QStringLiteral("--no-newline")}, QStringList()};
        }

        return ClipboardTool();
    }

    static bool setClipboardText(const ClipboardTool &tool, const QByteArray &bytes)
    {
        if (tool.program.isEmpty())
            return false;
        return runClipboardWriter(tool.program, tool.writeArgs, bytes);
    }

    static bool readClipboardText(const ClipboardTool &tool, QByteArray *out)
    {
        if (tool.program.isEmpty())
            return false;

        if (tool.program.endsWith(QStringLiteral("wl-copy")))
        {
            const QString wlPaste = QStandardPaths::findExecutable(QStringLiteral("wl-paste"));
            return !wlPaste.isEmpty() && runClipboardReader(wlPaste, tool.readArgs, out);
        }

        return runClipboardReader(tool.program, tool.readArgs, out);
    }

    static void uiMoveAbs(int fd, int x, int y)
    {
        uiWrite(fd, EV_ABS, ABS_X, x);
        uiWrite(fd, EV_ABS, ABS_Y, y);
        uiSync(fd);
    }

    static int openUinput()
    {
        int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (fd < 0)
            fd = ::open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
        if (fd < 0)
            LOG_ERROR("Failed to open uinput device, errno={}", errno);
        return fd;
    }

    static void updateScreenSize()
    {
        QScreen *sc = QGuiApplication::primaryScreen();
        if (sc)
        {
            qreal dpr = sc->devicePixelRatio();
            s_screenW = static_cast<int>(sc->geometry().width() * dpr);
            s_screenH = static_cast<int>(sc->geometry().height() * dpr);
        }
    }

    /* 获取（按需创建）uinput 虚拟键盘 fd */
    static int getKeyboardFd()
    {
        if (s_keyboardFd >= 0)
            return s_keyboardFd;

        s_keyboardFd = openUinput();
        if (s_keyboardFd < 0)
            return -1;

        ioctl(s_keyboardFd, UI_SET_EVBIT, EV_KEY);
        ioctl(s_keyboardFd, UI_SET_EVBIT, EV_SYN);
        ioctl(s_keyboardFd, UI_SET_EVBIT, EV_REP);

        /* 只开放键盘键位，避免混合设备被 libinput/桌面环境分类异常。 */
        for (int i = 1; i < BTN_MISC; ++i)
            ioctl(s_keyboardFd, UI_SET_KEYBIT, i);

        struct uinput_user_dev uidev;
        memset(&uidev, 0, sizeof(uidev));
        strncpy(uidev.name, "airan-desk virtual keyboard", UINPUT_MAX_NAME_SIZE - 1);
        uidev.id.bustype = BUS_USB;
        uidev.id.vendor = 0x1d6b;
        uidev.id.product = 0x0101;
        uidev.id.version = 1;

        if (::write(s_keyboardFd, &uidev, sizeof(uidev)) != static_cast<ssize_t>(sizeof(uidev)) ||
            ioctl(s_keyboardFd, UI_DEV_CREATE) < 0)
        {
            LOG_ERROR("Failed to create uinput virtual keyboard, errno={}", errno);
            ::close(s_keyboardFd);
            s_keyboardFd = -1;
            return -1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        LOG_INFO("Created uinput virtual keyboard device: fd={}", s_keyboardFd);
        return s_keyboardFd;
    }

    /* 获取（按需创建）uinput 虚拟鼠标 fd */
    static int getMouseFd()
    {
        if (s_mouseFd >= 0)
            return s_mouseFd;

        s_mouseFd = openUinput();
        if (s_mouseFd < 0)
            return -1;

        updateScreenSize();

        ioctl(s_mouseFd, UI_SET_EVBIT, EV_KEY);
        ioctl(s_mouseFd, UI_SET_EVBIT, EV_ABS);
        ioctl(s_mouseFd, UI_SET_EVBIT, EV_REL);
        ioctl(s_mouseFd, UI_SET_EVBIT, EV_SYN);
#ifdef UI_SET_PROPBIT
        ioctl(s_mouseFd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
#endif

        ioctl(s_mouseFd, UI_SET_KEYBIT, BTN_MOUSE);
        ioctl(s_mouseFd, UI_SET_KEYBIT, BTN_LEFT);
        ioctl(s_mouseFd, UI_SET_KEYBIT, BTN_RIGHT);
        ioctl(s_mouseFd, UI_SET_KEYBIT, BTN_MIDDLE);

        ioctl(s_mouseFd, UI_SET_ABSBIT, ABS_X);
        ioctl(s_mouseFd, UI_SET_ABSBIT, ABS_Y);
        ioctl(s_mouseFd, UI_SET_RELBIT, REL_WHEEL);

        struct uinput_user_dev uidev;
        memset(&uidev, 0, sizeof(uidev));
        strncpy(uidev.name, "airan-desk virtual mouse", UINPUT_MAX_NAME_SIZE - 1);
        uidev.id.bustype = BUS_USB;
        uidev.id.vendor = 0x1d6b;
        uidev.id.product = 0x0102;
        uidev.id.version = 1;
        uidev.absfuzz[ABS_X] = 0;
        uidev.absflat[ABS_X] = 0;
        uidev.absfuzz[ABS_Y] = 0;
        uidev.absflat[ABS_Y] = 0;
        uidev.absmin[ABS_X] = 0;
        uidev.absmax[ABS_X] = s_screenW - 1;
        uidev.absmin[ABS_Y] = 0;
        uidev.absmax[ABS_Y] = s_screenH - 1;

        if (::write(s_mouseFd, &uidev, sizeof(uidev)) != static_cast<ssize_t>(sizeof(uidev)) ||
            ioctl(s_mouseFd, UI_DEV_CREATE) < 0)
        {
            LOG_ERROR("Failed to create uinput virtual mouse, errno={}", errno);
            ::close(s_mouseFd);
            s_mouseFd = -1;
            return -1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        LOG_INFO("Created uinput virtual mouse device: screen={}x{}, fd={}", s_screenW, s_screenH, s_mouseFd);
        return s_mouseFd;
    }

} /* namespace */


void InputUtil::execKeyboardEvent(int keyCode, const QString &dwFlags)
{
    /* Linux 实现 (uinput 内核虚拟设备，X11/Wayland 均可用) */
    int fd = getKeyboardFd();
    if (fd < 0)
        return;

    int linuxKey = vkToLinux(keyCode);
    if (linuxKey < 0)
        return;

    LOG_DEBUG("Inject Linux keyboard event: keyCode={}, linuxKey={}, flags={}",
              keyCode, linuxKey, dwFlags.toStdString());
    uiWrite(fd, EV_KEY, static_cast<uint16_t>(linuxKey), dwFlags == "down" ? 1 : 0);
    uiWrite(fd, EV_SYN, SYN_REPORT, 0);
}

void InputUtil::execKeyboardText(const QString &text)
{
    if (text.isEmpty())
        return;

    int fd = getKeyboardFd();
    if (fd < 0)
        return;

    const ClipboardTool tool = findClipboardTool();
    if (tool.program.isEmpty())
    {
        LOG_WARN("Linux Unicode text injection requires wl-clipboard, xclip, or xsel: chars={}", text.size());
        return;
    }

    QByteArray previousClipboard;
    const bool hasPreviousClipboard = readClipboardText(tool, &previousClipboard);
    const QByteArray bytes = text.toUtf8();
    if (!setClipboardText(tool, bytes))
    {
        LOG_WARN("Failed to set clipboard for Linux text injection: chars={}", text.size());
        return;
    }

    pasteShortcut(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(180));

    if (hasPreviousClipboard)
        setClipboardText(tool, previousClipboard);

    LOG_DEBUG("Injected Linux keyboard text through clipboard paste: chars={}", text.size());
}

void InputUtil::execMouseEvent(int button, qreal x_n, qreal y_n, int mouseData, const QString &dwFlags)
{
    QScreen *screen = QGuiApplication::primaryScreen();
    QRect screenRect = screen->geometry();
    qreal scaleFactor = screen->devicePixelRatio();
    int x = static_cast<int>(x_n * screenRect.width() * scaleFactor);
    int y = static_cast<int>(y_n * screenRect.height() * scaleFactor);

    /* Linux 实现 (uinput 内核虚拟设备，X11/Wayland 均可用) */
    int fd = getMouseFd();
    if (fd < 0)
        return;

    updateScreenSize();
    x = std::clamp(x, 0, std::max(0, s_screenW - 1));
    y = std::clamp(y, 0, std::max(0, s_screenH - 1));

    LOG_DEBUG("Inject Linux mouse event: button={}, x={}, y={}, wheel={}, flags={}",
              button, x, y, mouseData, dwFlags.toStdString());

    /* 绝对鼠标设备只发送 ABS 坐标。不要再混发 REL_X/REL_Y，否则 libinput 会同时叠加相对移动，造成坐标漂移。 */
    uiMoveAbs(fd, x, y);

    if (dwFlags == "move")
    {
        return;
    }

    if (dwFlags == "wheel")
    {
        uiWrite(fd, EV_REL, REL_WHEEL, mouseData > 0 ? 1 : -1);
        uiSync(fd);
        return;
    }

    int btn;
    switch (button)
    {
    case Qt::LeftButton:
        btn = BTN_LEFT;
        break;
    case Qt::RightButton:
        btn = BTN_RIGHT;
        break;
    case Qt::MiddleButton:
        btn = BTN_MIDDLE;
        break;
    default:
        return;
    }

    if (dwFlags == "doubleClick")
    {
        uiWrite(fd, EV_KEY, static_cast<uint16_t>(btn), 1);
        uiSync(fd);
        uiWrite(fd, EV_KEY, static_cast<uint16_t>(btn), 0);
        uiSync(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        uiMoveAbs(fd, x, y);
        uiWrite(fd, EV_KEY, static_cast<uint16_t>(btn), 1);
        uiSync(fd);
        uiWrite(fd, EV_KEY, static_cast<uint16_t>(btn), 0);
        uiSync(fd);
    }
    else
    {
        uiWrite(fd, EV_KEY, static_cast<uint16_t>(btn), dwFlags == "down" ? 1 : 0);
        uiSync(fd);
    }
}
#endif /* Q_OS_LINUX */
