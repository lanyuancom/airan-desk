# Windows Build Guide

[简体中文](build_win.md) | English

## Requirements

1. Visual Studio 2019 or later
2. CMake 3.10 or later
3. Git
4. Qt 5.9 or later
5. OpenSSL 1.1.1
6. Prebuilt FFmpeg shared libraries

## Get the Source

```cmd
git clone <repository-url>
cd airan-desk
git submodule update --init --recursive
```

## Prepare Dependencies

### Qt

Install Qt 5.9 or later and select the matching MSVC component, for example:

- `C:/Qt/5.15.2/msvc2019`
- `C:/Qt/5.15.2/msvc2019_64`

### OpenSSL

- 32-bit: `C:/Program Files (x86)/OpenSSL-Win32`
- 64-bit: `C:/Program Files/OpenSSL-Win64`

### FFmpeg

Download the shared-library build that matches the target architecture and extract it to a custom directory, for example:

- `D:/lib/ffmpeg/ffmpeg-n7.1-latest-win32-lgpl-shared-7.1`
- `D:/lib/ffmpeg/ffmpeg-n7.1-latest-win64-lgpl-shared-7.1`

## Build with CMake Presets

> Requires CMake 3.21+.

### 32-bit

```cmd
cmake --preset win32-msvc-release
cmake --build --preset win32-msvc-release --config Release
```

### 64-bit

```cmd
cmake --preset win64-msvc-release
cmake --build --preset win64-msvc-release --config Release
```

## Build with Traditional CMake Commands

Use this path for CMake 3.10 to 3.20 compatibility.

### 32-bit

```cmd
mkdir out\build\win32-msvc-release
cd out\build\win32-msvc-release
cmake ../../.. -G "Visual Studio 16 2019" -A Win32 ^
 -DQt5_DIR=C:/Qt/5.15.2/msvc2019/lib/cmake/Qt5 ^
 -DOPENSSL_ROOT_DIR=C:/Program Files (x86)/OpenSSL-Win32 ^
 -DFFMPEG_ROOT_DIR=D:/lib/ffmpeg/ffmpeg-n7.1-latest-win32-lgpl-shared-7.1
cmake --build . --config Release
```

### 64-bit

```cmd
mkdir out\build\win64-msvc-release
cd out\build\win64-msvc-release
cmake ../../.. -G "Visual Studio 16 2019" -A x64 ^
 -DQt5_DIR=C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5 ^
 -DOPENSSL_ROOT_DIR=C:/Program Files/OpenSSL-Win64 ^
 -DFFMPEG_ROOT_DIR=D:/lib/ffmpeg/ffmpeg-n7.1-latest-win64-lgpl-shared-7.1
cmake --build . --config Release
```

## Output Directory

After the build completes, the executable is located at:

- `release/airan-desk.exe`

The build tries to copy these runtime dependencies into the output directory:

- Qt DLLs
- FFmpeg DLLs
- OpenSSL DLLs
- spdlog
- datachannel

## FAQ

### Qt5 Not Found

Check that `Qt5_DIR` points to `.../lib/cmake/Qt5`.

### Missing DLLs

Make sure Qt, FFmpeg, and OpenSSL match the target program architecture.

### `0xc000007b`

This is usually caused by mixing 32-bit and 64-bit dependencies.
