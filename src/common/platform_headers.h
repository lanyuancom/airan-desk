#ifndef PLATFORM_HEADERS_H
#define PLATFORM_HEADERS_H

#include <QtGlobal>

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef MY_D3D11_INCLUDE
#define MY_D3D11_INCLUDE
#include <d3d11.h>
#endif
#include <dxgi1_6.h>
#include <wrl.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#else
#include <dlfcn.h>
#endif

#endif /* PLATFORM_HEADERS_H */
