#include "screen_wake_util.h"
#include "config_util.h"

#if defined(Q_OS_MACOS)
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif

#if defined(Q_OS_LINUX)
#include <QProcess>
#endif

ScreenWakeUtilData* ScreenWakeUtilData::getInstance()
{
    static ScreenWakeUtilData instance;
    return &instance;
}

ScreenWakeUtilData::ScreenWakeUtilData(QObject *parent)
    : QObject(parent)
    , m_autoWakeEnabled(ConfigUtil->auto_wake_display)
    , m_preventSleepEnabled(ConfigUtil->prevent_sleep)
{
}

ScreenWakeUtilData::~ScreenWakeUtilData()
{
}

bool ScreenWakeUtilData::wakeDisplay()
{
    if (!m_autoWakeEnabled) {
        LOG_DEBUG("Auto wake display is disabled");
        return false;
    }
    
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_SHIFT;
    inputs[0].ki.dwFlags = 0;
    
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_SHIFT;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    
    const UINT result = SendInput(2, inputs, sizeof(INPUT));
    if (result == 2) {
        LOG_INFO("Windows display wake: simulated keyboard input sent");
        return true;
    } else {
        LOG_WARN("Windows display wake: failed to send keyboard input, result={}", result);
        return false;
    }
    
#elif defined(Q_OS_MACOS)
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    if (source) {
        CGEventRef event = CGEventCreateKeyboardEvent(source, 0, true);
        if (event) {
            CGEventPost(kCGHIDEventTap, event);
            CFRelease(event);
            LOG_INFO("macOS display wake: simulated keyboard event sent");
        }
        CFRelease(source);
    }
    
    IOPMAssertionID assertionID;
    IOReturn ret = IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventUserIdleDisplaySleep,
        kIOPMAssertionLevelOn,
        CFSTR("airan-desk wake display"),
        &assertionID
    );
    
    if (ret == kIOReturnSuccess) {
        usleep(100000);
        IOPMAssertionRelease(assertionID);
        LOG_INFO("macOS display wake: IOPMAssertion released");
    }
    
    return true;
    
#elif defined(Q_OS_LINUX)
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        QProcess::execute("wlrctl", QStringList() << "screencompositor" << "lock");
        LOG_INFO("Linux display wake: attempted Wayland lock/unlock");
    } else {
        QProcess::startDetached("xdotool", QStringList() << "key" << "Shift_L");
        LOG_INFO("Linux display wake: attempted X11 keyboard simulation");
    }
    return true;
    
#else
    LOG_WARN("Display wake not supported on this platform");
    return false;
#endif
}

bool ScreenWakeUtilData::preventSleep(bool enable)
{
    if (!m_preventSleepEnabled) {
        LOG_DEBUG("Prevent sleep is disabled");
        return false;
    }
    
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    if (enable) {
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        LOG_INFO("Windows: prevented system sleep and display timeout");
    } else {
        SetThreadExecutionState(ES_CONTINUOUS);
        LOG_INFO("Windows: restored default sleep policy");
    }
    return true;
    
#elif defined(Q_OS_MACOS)
    static IOPMAssertionID assertionID = 0;
    
    if (enable) {
        if (assertionID == 0) {
            IOReturn ret = IOPMAssertionCreateWithName(
                kIOPMAssertionTypePreventUserIdleSystemSleep,
                kIOPMAssertionLevelOn,
                CFSTR("airan-desk remote session"),
                &assertionID
            );
            if (ret == kIOReturnSuccess) {
                LOG_INFO("macOS: prevented system sleep during remote session");
            }
        }
    } else {
        if (assertionID != 0) {
            IOPMAssertionRelease(assertionID);
            assertionID = 0;
            LOG_INFO("macOS: restored default sleep policy");
        }
    }
    return true;
    
#elif defined(Q_OS_LINUX)
    if (enable) {
        QProcess::execute("xset", QStringList() << "s" << "off" << "dpms" << "force" << "on");
        LOG_INFO("Linux: disabled screen saver and display power management");
    } else {
        QProcess::execute("xset", QStringList() << "s" << "reset" << "dpms" << "auto");
        LOG_INFO("Linux: restored default screen saver and power management");
    }
    return true;
    
#else
    LOG_WARN("Prevent sleep not supported on this platform");
    return false;
#endif
}
