# Linux 编译指南

简体中文 | [English](build_linux.en.md)

## 支持范围

本文档覆盖 Linux 桌面端构建：

- `linux-x64`：主要验证路径，适合普通 Ubuntu/Debian x86_64 桌面。
- `linux-x86`：需要完整 32 位 Qt、FFmpeg、OpenSSL 与 multilib 环境。
- `linux-arm64` / `linux-armhf`：可原生构建，也可交叉编译；交叉编译时必须准备目标架构 sysroot 和依赖库。

项目 `CMakeLists.txt` 兼容 CMake 3.10+；`CMakePresets.json` 需要 CMake 3.21+。

## 基础依赖

### Ubuntu / Debian

```bash
sudo add-apt-repository universe
sudo add-apt-repository multiverse
sudo apt update
sudo apt install \
    build-essential \
    cmake \
    git \
    pkg-config \
    qtbase5-dev \
    libqt5svg5-dev \
    libqt5websockets5-dev \
    qttools5-dev \
    qttools5-dev-tools \
    libssl-dev \
    libavcodec-dev \
    libavdevice-dev \
    libavfilter-dev \
    libavformat-dev \
    libavutil-dev \
    libswresample-dev \
    libswscale-dev \
    libx11-dev \
    libxext-dev \
    libxfixes-dev \
    libxv-dev \
    libasound2-dev \
    zlib1g-dev
```

### Wayland 截屏可选依赖

X11 会话不需要安装 PipeWire 依赖。Wayland 会话如需 `pipewire` 截屏后端，Ubuntu 22.04+ 可安装：

```bash
sudo apt install \
    libpipewire-0.3-dev \
    libspa-0.2-dev \
    xdg-desktop-portal \
    xdg-desktop-portal-gtk
```

KDE 桌面可把 `xdg-desktop-portal-gtk` 换成 `xdg-desktop-portal-kde`。

CMake 会自动探测 `libpipewire-0.3` 和 Qt5 DBus。找到时启用 PipeWire 后端；找不到时构建不会失败，运行时回退到 Qt/X11 抓屏。

PipeWire 截屏不需要额外 udev 规则。它通过当前用户会话的 `xdg-desktop-portal` 请求授权；键盘鼠标注入仍由 `uinput` 规则控制。

### 其它发行版

包名会因发行版而异，核心依赖是：

- Qt5：Core、Gui、Svg、Widgets、WebSockets、Network；Wayland/PipeWire 后端还需要 DBus。
- FFmpeg：avcodec、avdevice、avfilter、avformat、avutil、swresample、swscale。
- OpenSSL、X11/Xext/Xfixes/Xv、ALSA、zlib、pthread/dl/rt/m。

## 获取源码

```bash
git clone <repository-url>
cd airan-desk
git submodule update --init --recursive
```

## 构建

### CMake 3.21+：使用 Preset

```bash
cmake --preset linux-x64
cmake --build --preset linux-x64 -j$(nproc)
```

其它预设：

```bash
cmake --preset linux-x86
cmake --build --preset linux-x86 -j$(nproc)

cmake --preset linux-arm64
cmake --build --preset linux-arm64 -j$(nproc)

cmake --preset linux-armhf
cmake --build --preset linux-armhf -j$(nproc)
```

### CMake 3.10+：传统命令

```bash
mkdir -p out/build/linux-x64
cd out/build/linux-x64
cmake ../../.. \
 -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_C_COMPILER=gcc \
 -DCMAKE_CXX_COMPILER=g++
make -j$(nproc)
```

如果 Qt 不在系统默认搜索路径，添加：

```bash
-DCMAKE_PREFIX_PATH=/path/to/qt5
```

## 32 位与 ARM 构建

### linux-x86

先安装 32 位工具链和目标架构依赖：

```bash
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install gcc-multilib g++-multilib libc6-dev-i386
```

构建示例：

```bash
mkdir -p out/build/linux-x86
cd out/build/linux-x86
cmake ../../.. \
 -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_C_COMPILER=gcc \
 -DCMAKE_CXX_COMPILER=g++ \
 -DCMAKE_C_FLAGS=-m32 \
 -DCMAKE_CXX_FLAGS=-m32 \
 -DCMAKE_EXE_LINKER_FLAGS=-m32 \
 -DCMAKE_SHARED_LINKER_FLAGS=-m32
make -j$(nproc)
```

### linux-arm64 / linux-armhf

原生 ARM 设备上可直接使用系统 `gcc/g++`。交叉编译时，先安装编译器：

```bash
sudo apt install \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
```

交叉编译还需要目标架构的 Qt、OpenSSL、FFmpeg 与 sysroot。仅指定交叉编译器通常不够，必要时传入：

```bash
-DCMAKE_FIND_ROOT_PATH=/path/to/sysroot
-DCMAKE_PREFIX_PATH=/path/to/target/qt5
```

低性能板端建议使用 `make -j1` 或 `make -j2`。

## 输出与安装

默认输出：

```bash
out/build/linux-x64/release/airan-desk
```

安装到系统：

```bash
sudo make install
sudo udevadm control --reload-rules
sudo udevadm trigger
```

`make install` 会安装可执行文件、桌面入口、配置文件、uinput 规则和 autostart 模板。

远程桌面建议在真实桌面会话内自启动，不建议使用 root/system 服务或 systemd 用户服务。桌面会话启动时会带上 `DISPLAY` / `WAYLAND_DISPLAY` / DBus / portal 等环境，截屏和输入注入最可靠。

为当前用户启用登录后自启动：

```bash
mkdir -p ~/.config/autostart
cp /usr/local/share/airan-desk/airan-desk-autostart.desktop ~/.config/autostart/airan-desk.desktop
```

立即按无界面模式启动：

```bash
/usr/local/bin/airan-desk --no-ui
```

取消当前用户自启动：

```bash
rm -f ~/.config/autostart/airan-desk.desktop
```

如果已经安装过旧版 systemd 服务，先清理旧服务和登录管理器用户下的残留进程：

```bash
systemctl --user disable --now airan-desk.service || true
sudo systemctl --global disable airan-desk.service
sudo rm -f /usr/local/share/systemd/user/airan-desk.service
sudo pkill -u gdm -f airan-desk || true
sudo pkill -u sddm -f airan-desk || true
sudo pkill -u lightdm -f airan-desk || true
```

如需开机后无人值守可远程桌面，建议配置显示管理器自动登录到目标桌面用户，然后依赖上面的 autostart 启动。以 GDM 为例，编辑 `/etc/gdm3/custom.conf` 或 `/etc/gdm/custom.conf`：

```ini
[daemon]
AutomaticLoginEnable=True
AutomaticLogin=wx
```

LightDM 可在 `/etc/lightdm/lightdm.conf` 或 `/etc/lightdm/lightdm.conf.d/*.conf` 中配置：

```ini
[Seat:*]
autologin-user=wx
autologin-user-timeout=0
```

自动登录会降低本机物理安全性。无人值守场景建议登录后自动锁屏，并结合磁盘加密、BIOS/UEFI 密码或受控物理环境使用。

## 运行配置

构建输出目录会复制：

- `conf/`
- `locale/`

运行前编辑 `conf/common.ini`：

```ini
[signal_server]
wsUrl = wss://your-signal-server.example/ws
```

启动：

```bash
./airan-desk
```

## 截屏后端

`conf/common.ini` 中的 `captureBackend` 控制 Linux 截屏后端：

| 取值 | 行为 |
| --- | --- |
| `auto` | 默认值。Wayland 会话优先 PipeWire，失败后回退 Qt；X11 会话使用 Qt |
| `qt` | 强制 Qt/X11 抓屏 |
| `pipewire` | 强制 xdg-desktop-portal + PipeWire；仅在编译时启用 PipeWire 后端后有效 |

Wayland 判定依据是 `XDG_SESSION_TYPE=wayland` 或 `WAYLAND_DISPLAY` 非空。首次使用 PipeWire 截屏时，桌面环境通常会弹出授权对话框。运行时需要有效的用户会话 DBus 和 portal 后端；如果通过 SSH 或 systemd 服务启动，通常不会有可交互的授权窗口。

## 常见问题

### `qsizetype has not been declared`

这是 Qt 5.9 环境常见的兼容问题。当前代码已避免在项目源码中直接使用 `qsizetype`；如果仍出现该错误，请确认源码已更新并清理旧构建目录后重新配置：

```bash
rm -rf out/build/linux-x64
cmake --preset linux-x64
cmake --build --preset linux-x64 -j$(nproc)
```

### 找不到 Qt5 Svg 或 WebSockets

确认已安装：

```bash
sudo apt install libqt5svg5-dev libqt5websockets5-dev
```

### Wayland 下没有 PipeWire 后端

确认配置阶段有 `PipeWire screen-capture backend enabled`。如果没有，安装 PipeWire/portal 开发包后重新运行 CMake。

如果已启用但运行时无法抓屏，检查 portal 服务和会话环境：

```bash
echo $XDG_SESSION_TYPE
echo $WAYLAND_DISPLAY
systemctl --user status xdg-desktop-portal
systemctl --user status pipewire
```

GNOME 通常需要 `xdg-desktop-portal-gnome` 或 `xdg-desktop-portal-gtk`；KDE 通常需要 `xdg-desktop-portal-kde`。
