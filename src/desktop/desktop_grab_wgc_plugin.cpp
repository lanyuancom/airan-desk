#include "desktop_capture_plugin.h"
#include "desktop_grab_wgc.h"

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)

#include <winrt/Windows.Graphics.Capture.h>

extern "C" __declspec(dllexport) DesktopGrab *airanCreateDesktopGrabber()
{
    return new DesktopGrabWGC();
}

extern "C" __declspec(dllexport) void airanDestroyDesktopGrabber(DesktopGrab *grabber)
{
    delete grabber;
}

extern "C" __declspec(dllexport) bool airanIsDesktopGrabberSupported()
{
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    const bool hasD3d11 = d3d11 && GetProcAddress(d3d11, "D3D11CreateDevice");
    if (d3d11)
        FreeLibrary(d3d11);
    if (!hasD3d11)
        return false;

    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (...)
    {
    }

    try
    {
        return winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported();
    }
    catch (...)
    {
        return false;
    }
}

#endif
