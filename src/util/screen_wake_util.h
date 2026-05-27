#ifndef SCREEN_WAKE_UTIL_H
#define SCREEN_WAKE_UTIL_H

#include <QObject>
#include <spdlog/spdlog.h>

#define ScreenWakeUtil ScreenWakeUtilData::getInstance()

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#include <windows.h>
#endif

class ScreenWakeUtilData : public QObject
{
    Q_OBJECT
public:
    static ScreenWakeUtilData* getInstance();
    
    bool wakeDisplay();
    bool preventSleep(bool enable);
    
    void setAutoWakeEnabled(bool enabled) { m_autoWakeEnabled = enabled; }
    bool isAutoWakeEnabled() const { return m_autoWakeEnabled; }
    
    void setPreventSleepEnabled(bool enabled) { m_preventSleepEnabled = enabled; }
    bool isPreventSleepEnabled() const { return m_preventSleepEnabled; }
    
private:
    explicit ScreenWakeUtilData(QObject *parent = nullptr);
    ~ScreenWakeUtilData();
    Q_DISABLE_COPY(ScreenWakeUtilData)
    
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    static const DWORD ES_CONTINUOUS = 0x80000000;
    static const DWORD ES_SYSTEM_REQUIRED = 0x00000001;
    static const DWORD ES_DISPLAY_REQUIRED = 0x00000002;
    static const UINT WM_SYSCOMMAND = 0x0112;
    static const UINT SC_MONITORPOWER = 0xF170;
#endif
    
    bool m_autoWakeEnabled;
    bool m_preventSleepEnabled;
};

#endif
