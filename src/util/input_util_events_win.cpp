/* Platform input event implementation split from input_util_events.cpp. */

#include "input_util.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <cmath>

#include "../common/logger_manager.h"

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#include <windows.h>
#include <shellapi.h>

namespace
{
    LONG normalizeAbsoluteCoordinate(int coord, int max)
    {
        if (max <= 1)
        {
            return 0;
        }
        double value = (coord * 65535.0) / (static_cast<double>(max) - 1.0);
        if (value < 0.0)
        {
            value = 0.0;
        }
        if (value > 65535.0)
        {
            value = 65535.0;
        }
        return static_cast<LONG>(std::round(value));
    }
} /* namespace */

void InputUtil::execKeyboardEvent(int keyCode, const QString &dwFlags)
{
    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(keyCode);
    input.ki.dwFlags = dwFlags == "down" ? 0 : KEYEVENTF_KEYUP;

    SendInput(1, &input, sizeof(INPUT));
}

void InputUtil::execKeyboardText(const QString &text)
{
    if (text.isEmpty())
    {
        return;
    }
    const std::wstring value = text.toStdWString();
    for (wchar_t ch : value)
    {
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = static_cast<WORD>(ch);
        inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
        inputs[1] = inputs[0];
        inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, inputs, sizeof(INPUT));
    }
}

void InputUtil::execMouseEvent(int button, qreal x_n, qreal y_n, int mouseData, const QString &dwFlags)
{
    QScreen *screen = QGuiApplication::primaryScreen();
    QRect screenRect = screen->geometry();
    qreal scaleFactor = screen->devicePixelRatio();
    int x = static_cast<int>(x_n * screenRect.width() * scaleFactor);
    int y = static_cast<int>(y_n * screenRect.height() * scaleFactor);

    /* Windows 实现 */
    /* 使用绝对坐标并修正整数除法问题，确保 SendInput 在各种场景（包括 Task Manager）下更可靠 */
    int screenX = GetSystemMetrics(SM_CXSCREEN);
    int screenY = GetSystemMetrics(SM_CYSCREEN);

    LONG absX = normalizeAbsoluteCoordinate(x, screenX);
    LONG absY = normalizeAbsoluteCoordinate(y, screenY);

    /* 先移动鼠标到目标物理位置（兼容部分只响应 SetCursorPos 的情况） */
    SetCursorPos(x, y);

    /* 仅移动，不发送点击 */
    if (dwFlags == "move")
    {
        INPUT input = {0};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        input.mi.dx = absX;
        input.mi.dy = absY;
        SendInput(1, &input, sizeof(INPUT));
        return;
    }

    /* 滚轮事件：只发送 WHEEL 类型并设置 mouseData */
    if (dwFlags == "wheel")
    {
        INPUT input = {0};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = mouseData;
        SendInput(1, &input, sizeof(INPUT));
        return;
    }

    /* 双击：发送两次 按下/抬起，带绝对坐标 */
    if (dwFlags == "doubleClick")
    {
        INPUT inputs[4] = {0};
        DWORD downFlag = (button == Qt::LeftButton) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
        DWORD upFlag = (button == Qt::LeftButton) ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;

        /* 第一次按下 */
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = downFlag | MOUSEEVENTF_ABSOLUTE;
        inputs[0].mi.dx = absX;
        inputs[0].mi.dy = absY;
        /* 第一次抬起 */
        inputs[1] = inputs[0];
        inputs[1].mi.dwFlags = upFlag | MOUSEEVENTF_ABSOLUTE;
        /* 第二次按下 */
        inputs[2] = inputs[0];
        inputs[2].mi.dwFlags = downFlag | MOUSEEVENTF_ABSOLUTE;
        /* 第二次抬起 */
        inputs[3] = inputs[1];

        SendInput(4, inputs, sizeof(INPUT));
        return;
    }

    /* 普通按下/抬起，带绝对坐标 */
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    DWORD flag = 0;
    switch (button)
    {
    case Qt::LeftButton:
        flag = (dwFlags == "down") ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case Qt::RightButton:
        flag = (dwFlags == "down") ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case Qt::MiddleButton:
        flag = (dwFlags == "down") ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    default:
        return;
    }

    input.mi.dwFlags = flag | MOUSEEVENTF_ABSOLUTE;
    input.mi.dx = absX;
    input.mi.dy = absY;
    SendInput(1, &input, sizeof(INPUT));
}
#endif
