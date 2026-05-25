# macOS Build Guide

[简体中文](build_mac.md) | English

> The project has basic macOS build and input support, but it is still best treated as an internal test build: build it, grant permissions, verify behavior, then consider signing and notarization.

## Current macOS Support

- Basic Qt / CMake build branch
- Mouse and keyboard injection through CoreGraphics
- Generic desktop capture fallback through Qt `QScreen::grabWindow(0)`
- Best suited for:
  - Local self-testing
  - Small-scale internal testing
  - Pre-release validation before signing and notarization

## Known Limitations

1. System permissions must be granted manually, otherwise:
   - The app may start but screen capture can be black or fail
   - The desktop may be visible but keyboard and mouse control may not work
2. Unsigned builds may be blocked by Gatekeeper on first launch.
3. Retina and multi-monitor scenarios should be verified on real hardware for coordinate accuracy.

## Dependencies

Recommended environment:

- macOS 12+
- Xcode Command Line Tools
- CMake 3.10+
- Qt 5.15.x
- FFmpeg
- OpenSSL

Recommended Homebrew setup:

```bash
xcode-select --install
brew install cmake openssl@3 ffmpeg
brew install qt@5
```

If you already installed Qt through the official Qt installer, you can use that path instead of Homebrew Qt.

## Configure

### Option 1: Official Qt Install Directory

```bash
cmake -S . -B out/build/macos \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/5.15.2/clang_64 \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
  -DFFMPEG_ROOT_DIR=/opt/homebrew/opt/ffmpeg
```

### Option 2: Homebrew Qt5

On Intel Macs this is usually similar to:

```bash
cmake -S . -B out/build/macos \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=$(brew --prefix qt@5) \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
  -DFFMPEG_ROOT_DIR=$(brew --prefix ffmpeg)
```

On Apple Silicon, use the path reported by `brew --prefix` on your machine.

## Build

```bash
cmake --build out/build/macos --config Release -j$(sysctl -n hw.ncpu)
```

## Deploy the `.app`

The project enables `MACOSX_BUNDLE` in CMake.

After building, try:

```bash
cmake --build out/build/macos --target mac_bundle --config Release
```

If `macdeployqt` exists, Qt dependencies are deployed into the `.app` automatically. Otherwise, run a command similar to:

```bash
/path/to/Qt/5.15.2/clang_64/bin/macdeployqt out/build/macos/release/airan-desk.app
```

## First Launch Permissions

This is a remote desktop tool. On macOS you must handle at least these permissions:

### 1. Screen Recording

Go to:

- **System Settings -> Privacy & Security -> Screen Recording**

Allow `airan-desk.app`.

Without this permission the remote side may show:

- Black screen
- Capture failure
- Window frames without actual content

### 2. Accessibility

Go to:

- **System Settings -> Privacy & Security -> Accessibility**

Allow `airan-desk.app`.

Without this permission, remote input control may not work.

### 3. Input Monitoring

Depending on the macOS version and scenario, you may also need:

- **System Settings -> Privacy & Security -> Input Monitoring**

Check this permission if keyboard input is unstable.

## Opening an Unsigned Build

If you do not have an Apple Developer account, you can still use the app for development and internal testing.

If first launch is blocked:

1. Right-click the app in Finder and choose **Open**
2. Or go to:
   - **System Settings -> Privacy & Security -> Open Anyway**

If the app was downloaded from the network, it may carry a quarantine flag. You can remove it manually:

```bash
xattr -dr com.apple.quarantine /path/to/airan-desk.app
```

> Only run this command for apps you trust.

## Run Notes

Before launching, make sure the `.app` or executable directory contains:

- `conf/`
- `locale/`

Configure:

```ini
[signal_server]
wsUrl=wss://your-signal-server.example/ws
```

## Troubleshooting

### App Starts but Desktop Is Not Visible

Check:

- Screen Recording permission is enabled
- The app was restarted after permission changes

### Desktop Is Visible but Control Does Not Work

Check:

- Accessibility permission is enabled
- Input Monitoring permission is enabled if needed

### Mouse Coordinates Are Incorrect

Check:

- Retina display
- Multi-monitor layout
- Whether the primary/secondary display arrangement changed

These issues usually require real-device coordinate mapping adjustments.
