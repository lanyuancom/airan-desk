# Linux Build Guide

[简体中文](build_linux.md) | English

## Support Scope

This guide covers Linux desktop builds:

- `linux-x64`: the primary path for common Ubuntu/Debian x86_64 desktops.
- `linux-x86`: requires a complete 32-bit Qt, FFmpeg, OpenSSL, and multilib setup.
- `linux-arm64` / `linux-armhf`: can be built natively or cross-compiled; cross builds require a matching sysroot and target dependencies.

`CMakeLists.txt` supports CMake 3.10+. `CMakePresets.json` requires CMake 3.21+.

## Dependencies

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

### Optional Wayland Capture Dependencies

X11 sessions do not need PipeWire dependencies. For the `pipewire` capture backend on Wayland, install these on Ubuntu 22.04+:

```bash
sudo apt install \
    libpipewire-0.3-dev \
    libspa-0.2-dev \
    xdg-desktop-portal \
    xdg-desktop-portal-gtk
```

Use `xdg-desktop-portal-kde` instead of `xdg-desktop-portal-gtk` on KDE.

CMake probes `libpipewire-0.3` and Qt5 DBus automatically. If found, the PipeWire backend is enabled; otherwise the build still succeeds and runtime capture falls back to Qt/X11.

PipeWire capture does not need an extra udev rule. It requests permission through `xdg-desktop-portal` in the current user session; keyboard and mouse injection are still controlled by the `uinput` rule.

### Other Distributions

Package names vary by distro. The required components are:

- Qt5: Core, Gui, Svg, Widgets, WebSockets, Network; the Wayland/PipeWire backend also needs DBus.
- FFmpeg: avcodec, avdevice, avfilter, avformat, avutil, swresample, swscale.
- OpenSSL, X11/Xext/Xfixes/Xv, ALSA, zlib, pthread/dl/rt/m.

## Get the Source

```bash
git clone <repository-url>
cd airan-desk
git submodule update --init --recursive
```

## Build

### CMake 3.21+: Presets

```bash
cmake --preset linux-x64
cmake --build --preset linux-x64 -j$(nproc)
```

Other presets:

```bash
cmake --preset linux-x86
cmake --build --preset linux-x86 -j$(nproc)

cmake --preset linux-arm64
cmake --build --preset linux-arm64 -j$(nproc)

cmake --preset linux-armhf
cmake --build --preset linux-armhf -j$(nproc)
```

### CMake 3.10+: Traditional Commands

```bash
mkdir -p out/build/linux-x64
cd out/build/linux-x64
cmake ../../.. \
 -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_C_COMPILER=gcc \
 -DCMAKE_CXX_COMPILER=g++
make -j$(nproc)
```

If Qt is not in the default search path, add:

```bash
-DCMAKE_PREFIX_PATH=/path/to/qt5
```

## 32-bit and ARM Builds

### linux-x86

Install 32-bit toolchain support and target dependencies first:

```bash
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install gcc-multilib g++-multilib libc6-dev-i386
```

Build example:

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

On native ARM devices, use the system `gcc/g++`. For cross builds, install cross compilers:

```bash
sudo apt install \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
```

Cross builds also need target-architecture Qt, OpenSSL, FFmpeg, and a sysroot. Passing only the cross compiler is usually not enough. Add these when needed:

```bash
-DCMAKE_FIND_ROOT_PATH=/path/to/sysroot
-DCMAKE_PREFIX_PATH=/path/to/target/qt5
```

On low-power boards, prefer `make -j1` or `make -j2`.

## Output and Install

Default output:

```bash
out/build/linux-x64/release/airan-desk
```

Install system-wide:

```bash
sudo make install
sudo udevadm control --reload-rules
sudo udevadm trigger
```

`make install` installs the executable, desktop entry, config files, uinput rules, and an autostart template.

For remote desktop, prefer starting inside the real desktop session. Do not use a root/system service or a systemd user service for desktop capture. A desktop-session autostart inherits `DISPLAY` / `WAYLAND_DISPLAY` / DBus / portal environment, which gives screen capture and input injection the best chance of working reliably.

Enable login autostart for the current user:

```bash
mkdir -p ~/.config/autostart
cp /usr/local/share/airan-desk/airan-desk-autostart.desktop ~/.config/autostart/airan-desk.desktop
```

Start immediately in headless mode:

```bash
/usr/local/bin/airan-desk --no-ui
```

Disable autostart for the current user:

```bash
rm -f ~/.config/autostart/airan-desk.desktop
```

If an older systemd service was installed before, clean it up first:

```bash
systemctl --user disable --now airan-desk.service || true
sudo systemctl --global disable airan-desk.service
sudo rm -f /usr/local/share/systemd/user/airan-desk.service
sudo pkill -u gdm -f airan-desk || true
sudo pkill -u sddm -f airan-desk || true
sudo pkill -u lightdm -f airan-desk || true
```

For unattended remote desktop after reboot, configure the display manager to auto-login to the target desktop user, then rely on the autostart entry above. For GDM, edit `/etc/gdm3/custom.conf` or `/etc/gdm/custom.conf`:

```ini
[daemon]
AutomaticLoginEnable=True
AutomaticLogin=wx
```

For LightDM, configure `/etc/lightdm/lightdm.conf` or `/etc/lightdm/lightdm.conf.d/*.conf`:

```ini
[Seat:*]
autologin-user=wx
autologin-user-timeout=0
```

Auto-login lowers physical security. For unattended setups, consider locking the session after login and using disk encryption, BIOS/UEFI passwords, or a controlled physical environment.

## Runtime Configuration

The build output copies:

- `conf/`
- `locale/`

Before running, edit `conf/common.ini`:

```ini
[signal_server]
wsUrl = wss://your-signal-server.example/ws
```

Start:

```bash
./airan-desk
```

## Capture Backend

`conf/common.ini` controls Linux capture through `captureBackend`:

| Value | Behavior |
| --- | --- |
| `auto` | Default. Wayland sessions prefer PipeWire and fall back to Qt; X11 sessions use Qt |
| `qt` | Force Qt/X11 capture |
| `pipewire` | Force xdg-desktop-portal + PipeWire; only effective when the build enabled the PipeWire backend |

Wayland is detected through `XDG_SESSION_TYPE=wayland` or a non-empty `WAYLAND_DISPLAY`. The first PipeWire capture normally opens a desktop permission dialog. Runtime capture requires a valid user session D-Bus and portal backend; SSH or systemd-service launches usually do not have an interactive permission dialog.

## Troubleshooting

### `qsizetype has not been declared`

This is common on Qt 5.9. Current project sources avoid direct `qsizetype` usage; if you still see this error, update the source and reconfigure from a clean build directory:

```bash
rm -rf out/build/linux-x64
cmake --preset linux-x64
cmake --build --preset linux-x64 -j$(nproc)
```

### Qt5 Svg or WebSockets Not Found

Install:

```bash
sudo apt install libqt5svg5-dev libqt5websockets5-dev
```

### PipeWire Backend Missing on Wayland

Check whether configure output contains `PipeWire screen-capture backend enabled`. If not, install the PipeWire/portal development packages and rerun CMake.

If the backend is enabled but runtime capture still fails, check the portal services and session environment:

```bash
echo $XDG_SESSION_TYPE
echo $WAYLAND_DISPLAY
systemctl --user status xdg-desktop-portal
systemctl --user status pipewire
```

GNOME usually needs `xdg-desktop-portal-gnome` or `xdg-desktop-portal-gtk`; KDE usually needs `xdg-desktop-portal-kde`.
