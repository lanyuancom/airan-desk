/* Platform input event implementation split from input_util_events.cpp. */

#include "input_util.h"

#if !defined(Q_OS_WIN64) && !defined(Q_OS_WIN32) && !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
void InputUtil::execKeyboardEvent(int, const QString &)
{
}

void InputUtil::execKeyboardText(const QString &)
{
}

void InputUtil::execMouseEvent(int, qreal, qreal, int, const QString &)
{
}
#endif
