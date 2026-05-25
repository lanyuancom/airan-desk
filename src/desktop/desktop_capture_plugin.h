#ifndef DESKTOP_CAPTURE_PLUGIN_H
#define DESKTOP_CAPTURE_PLUGIN_H

class DesktopGrab;

extern "C"
{
    typedef DesktopGrab *(*airanCreateDesktopGrabberFn)();
    typedef void (*airanDestroyDesktopGrabberFn)(DesktopGrab *);
    typedef bool (*airanIsDesktopGrabberSupportedFn)();
}

#define AIRAN_CAPTURE_PLUGIN_CREATE "airanCreateDesktopGrabber"
#define AIRAN_CAPTURE_PLUGIN_DESTROY "airanDestroyDesktopGrabber"
#define AIRAN_CAPTURE_PLUGIN_SUPPORTED "airanIsDesktopGrabberSupported"

#endif /* DESKTOP_CAPTURE_PLUGIN_H */
