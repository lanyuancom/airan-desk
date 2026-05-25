#ifndef DESKTOP_GRAB_PIPEWIRE_H
#define DESKTOP_GRAB_PIPEWIRE_H

/* Wayland / xdg-desktop-portal + PipeWire screen-capture backend.
 * Active only when AIRAN_HAS_PIPEWIRE is defined at configure time. */

#include "desktop_grab.h"

#ifdef AIRAN_HAS_PIPEWIRE

#include <QImage>
#include <QMutex>
#include <QString>
#include <QWaitCondition>
#include <atomic>
#include <memory>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_hook;

class DesktopGrabPipeWire : public DesktopGrab
{
    Q_OBJECT
public:
    explicit DesktopGrabPipeWire(QObject *parent = nullptr);
    ~DesktopGrabPipeWire() override;

    bool init(int screenIndex) override;
    bool grabFrameCPU(QImage &outImage) override;
    void stopCapture() override;

    /* Static probe for factory: check whether PipeWire libs load at runtime. */
    static bool isAvailable();

private:
    bool startPortalSession();
    bool connectPipeWire();
    void teardown();

    /* PipeWire stream callbacks (static thunks). */
    static void onStreamProcessStatic(void *userdata);
    static void onStreamParamChangedStatic(void *userdata, uint32_t id, const struct spa_pod *param);
    static void onStreamStateChangedStatic(void *userdata, int oldState, int newState, const char *error);

    void onStreamProcess();
    void onStreamParamChanged(uint32_t id, const struct spa_pod *param);

    /* PipeWire state */
    pw_thread_loop *m_loop{nullptr};
    pw_context *m_context{nullptr};
    pw_core *m_core{nullptr};
    pw_stream *m_stream{nullptr};
    spa_hook *m_streamListener{nullptr};
    int m_pwFd{-1};
    uint32_t m_nodeId{0};

    /* Frame buffer */
    QMutex m_frameMutex;
    QImage m_latest;
    int m_frameWidth{0};
    int m_frameHeight{0};
    int m_frameStride{0};
    uint32_t m_frameFormat{0};
    std::atomic<bool> m_haveFrame{false};
    std::atomic<bool> m_stopping{false};

    QString m_portalSessionPath;
    QString m_portalRequestToken;
};

#endif /* AIRAN_HAS_PIPEWIRE */

#endif /* DESKTOP_GRAB_PIPEWIRE_H */
