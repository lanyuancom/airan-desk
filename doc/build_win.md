# Windows 编译指南

简体中文 | [English](build_win.en.md)

## 环境要求

1. Visual Studio 2019 或更高版本
2. CMake 3.10 或更高版本
3. Git
4. Qt 5.9 或更高版本
5. OpenSSL 1.1.1
6. FFmpeg 预编译共享库

## 获取源码

```cmd
git clone <repository-url>
cd airan-desk
git submodule update --init --recursive
```

## 依赖准备

### Qt

安装 Qt 5.9 及以上版本，并选择对应 MSVC 组件，例如：

- `C:/Qt/5.15.2/msvc2019`
- `C:/Qt/5.15.2/msvc2019_64`

### OpenSSL

- 32 位：`C:/Program Files (x86)/OpenSSL-Win32`
- 64 位：`C:/Program Files/OpenSSL-Win64`

### FFmpeg

下载与目标架构对应的共享库版本，解压到自定义目录，例如：

- `D:/lib/ffmpeg/ffmpeg-n7.1-latest-win32-lgpl-shared-7.1`
- `D:/lib/ffmpeg/ffmpeg-n7.1-latest-win64-lgpl-shared-7.1`

## 使用 CMake Presets 构建

> 需要 CMake 3.21+

### 32 位

```cmd
cmake --preset win32-msvc-release
cmake --build --preset win32-msvc-release --config Release
```

### 64 位

```cmd
cmake --preset win64-msvc-release
cmake --build --preset win64-msvc-release --config Release
```

## 使用传统 CMake 命令构建（兼容 CMake 3.10）

### 32 位

```cmd
mkdir out\build\win32-msvc-release
cd out\build\win32-msvc-release
cmake ../../.. -G "Visual Studio 16 2019" -A Win32 ^
 -DQt5_DIR=C:/Qt/5.15.2/msvc2019/lib/cmake/Qt5 ^
 -DOPENSSL_ROOT_DIR=C:/Program Files (x86)/OpenSSL-Win32 ^
 -DFFMPEG_ROOT_DIR=D:/lib/ffmpeg/ffmpeg-n7.1-latest-win32-lgpl-shared-7.1
cmake --build . --config Release
```

### 64 位

```cmd
mkdir out\build\win64-msvc-release
cd out\build\win64-msvc-release
cmake ../../.. -G "Visual Studio 16 2019" -A x64 ^
 -DQt5_DIR=C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5 ^
 -DOPENSSL_ROOT_DIR=C:/Program Files/OpenSSL-Win64 ^
 -DFFMPEG_ROOT_DIR=D:/lib/ffmpeg/ffmpeg-n7.1-latest-win64-lgpl-shared-7.1
cmake --build . --config Release
```

## 输出目录

构建完成后，可执行文件位于对应构建目录下的：

- `release/airan-desk.exe`

程序构建后会自动尝试复制以下依赖到输出目录：

- Qt DLL
- FFmpeg DLL
- OpenSSL DLL
- spdlog
- datachannel

## 常见问题

### 找不到 Qt5

请检查 `Qt5_DIR` 是否指向 `.../lib/cmake/Qt5`。

### 缺少 DLL

请确认 Qt、FFmpeg、OpenSSL 与目标程序架构一致。

### `0xc000007b`

通常是 32/64 位依赖混用导致。
