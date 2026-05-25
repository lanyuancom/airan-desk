# airan-desk

简体中文 | [English](README.en.md)

airan-desk 是一个基于 WebRTC 的远程桌面控制应用，支持桌面画面传输、远程键鼠控制、文件传输和远程终端，当前主要面向 Windows、Linux 与 macOS 平台。

## 项目简介

- 基于 **Qt5 + WebRTC(libdatachannel) + FFmpeg** 实现
- 支持远程桌面查看与控制
- 支持文件传输
- 支持远程终端，使用 Windows ConPTY / Linux 与 macOS forkpty 启动系统 shell，并使用原生 `libvterm + Qt Widgets` 渲染全屏 TUI
- 支持 Windows 零拷贝编码相关能力（依赖显卡驱动与平台能力）

> 说明：`CMakePresets.json` 仍然需要 **CMake 3.21+**。如果你的 CMake 版本是 3.10 ~ 3.20，请使用传统 `cmake` 命令方式构建。

## 平台能力矩阵

| 平台 / 会话 | 截屏后端 | 帧率上限 | 说明 |
| --- | --- | --- | --- |
| Windows 10 1903+（build ≥ 18362） | WGC（GPU 零拷贝）→ Qt | 不限 | 优先加载 `airan_capture_wgc` 插件，失败回落 Qt |
| Windows 7 / 8 / 8.1 / 早期 Win10 | Qt（CPU GDI） | **15 fps** | WGC 不可用，自动降帧避免 CPU 过载 |
| Linux X11 | Qt（X11 抓屏） | 不限 | 默认路径，不依赖 portal |
| Linux Wayland | PipeWire + xdg-desktop-portal → Qt | 不限 | `auto` 模式下检测到 `WAYLAND_DISPLAY` / `XDG_SESSION_TYPE=wayland` 即启用；首次会话弹出 portal 授权对话框 |
| macOS | Qt | 不限 | 需手动授权"屏幕录制"与"辅助功能"权限 |

`conf/common.ini` 的 `captureBackend` 可显式指定：`auto`（默认）/ `wgc` / `qt` / `pipewire`。非法值回退为 `auto`。

> Wayland 后端为编译期可选，需要构建机能找到 `libpipewire-0.3` 与 Qt5 DBus；找不到时 CMake 会打印 `PipeWire screen-capture backend disabled` 并自动回落到 Qt+X11。

## 已测试环境

### Windows 7 32 位

- FFmpeg 7.1.3 + Qt 5.9.9
- FFmpeg 7.1.3 + Qt 5.15.2

### Windows 10 64 位

- FFmpeg 7.1.3 + Qt 5.9.9
- FFmpeg 7.1.3 + Qt 5.15.2

### Ubuntu 18.04 armhf

- FFmpeg 4.1.4 + Qt 5.9.5（apt 安装）

### Ubuntu 22.04 x64

- FFmpeg 4.4.2 + Qt 5.15.3（apt 安装）

## 界面预览

![启动界面](resource/images/main_window.png)
![控制界面](resource/images/control_window.png)
![文件传输界面](resource/images/file_transfer_window.png)
![终端界面: linux](resource/images/terminal_window_linux.png)
![终端界面: win7](resource/images/terminal_window_win7.png)
![终端界面: win10](resource/images/terminal_window_win10.png)

## 构建文档

- [Windows 编译指南](doc/build_win.md)
- [Linux 编译指南（含 x86 / x64 / arm64 / armhf）](doc/build_linux.md)
- [macOS 编译指南](doc/build_mac.md)

Linux 文档已统一整理为：

- `linux-x86`
- `linux-x64`
- `linux-arm64`
- `linux-armhf`

## 使用说明

### 启动前准备

1. 进入程序输出目录。
2. 确认以下资源已存在：
   - `conf/config.ini`
   - `locale/`
3. 编辑 `conf/config.ini`，至少配置：
   - `signal_server.wsUrl=你的信令服务器地址`

示例：

```ini
[signal_server]
wsUrl=wss://your-signal-server.example/ws
```

### 多语言

可在 `conf/common.ini` 中配置界面语言：

```ini
[local]
language = auto
```

支持：

- `auto`：跟随系统语言，中文系统使用简体中文，其他系统使用英文
- `zh_CN`：简体中文
- `en_US`：英文

### 启动程序

- Windows：运行 `release\airan-desk.exe`
- Linux：在输出目录运行 `./airan-desk`
- macOS：优先运行 `airan-desk.app`

### macOS 额外说明

macOS 版本当前仅按 Qt/macOS 常规方式提供构建脚本与代码适配，尚未在真实 macOS 设备上完成实机测试。首次运行通常还需要手动完成：

- 屏幕录制权限授权
- 辅助功能权限授权
- 未签名应用放行

详见 [macOS 编译指南](doc/build_mac.md)。

### 基本使用流程

1. 启动被控端和控制端程序。
2. 确保双方都可以访问同一个信令服务器。
3. 在控制端输入目标连接信息并发起连接。
4. 连接成功后即可进行远程桌面查看、键鼠控制。
5. 如需传输文件，可打开文件传输窗口进行发送。
6. 如需远程命令行，可选择“终端”模式连接；`vim`、`top`、`htop` 等全屏 TUI 会通过 PTY/ConPTY 与原生终端模拟器渲染。

## 依赖说明

主要依赖如下：

- [Qt5](https://www.qt.io/)：跨平台 GUI、网络与本地化基础库，感谢 Qt Company 与 Qt 社区
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel)：WebRTC 数据通道与媒体传输能力，感谢 Paul-Louis Ageneau 与贡献者
- [spdlog](https://github.com/gabime/spdlog)：日志库，感谢 Gabi Melman 与贡献者
- [FFmpeg](https://github.com/FFmpeg/FFmpeg)：音视频编解码与格式处理能力，感谢 FFmpeg 项目团队与贡献者
- [libvterm](https://github.com/neovim/libvterm)：终端控制序列解析与屏幕状态维护，感谢 Paul Evans、Neovim 维护者与贡献者

远程终端前端是 Qt Widgets 原生控件，不依赖 `QtWebEngine`。

第三方源码通过 Git submodule 管理。首次克隆或更新依赖时请执行：

```bash
git submodule update --init --recursive
```

## 许可证

详见 [LICENSE](LICENSE)。
