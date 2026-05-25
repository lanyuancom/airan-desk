# macOS 编译说明

简体中文 | [English](build_mac.en.md)

> 当前项目已补齐基础的 macOS 构建入口与输入实现，但 **仍建议先按“内测版”思路使用**：先编译、先授权、先验证，再考虑签名与公证。

## 当前 macOS 支持现状

- 已支持 Qt / CMake 基础构建分支
- 已提供 macOS 鼠标、键盘事件注入实现（CoreGraphics）
- 已提供通用桌面采集回退实现（Qt `QScreen::grabWindow(0)`）
- 当前更适合作为：
  - 本机自测版本
  - 小范围内测版本
  - 后续签名/公证前的预发布版本

## 已知限制

1. **必须手动授权系统权限**，否则会出现：
   - 能启动，但抓屏黑屏/失败
   - 能看到画面，但无法控制鼠标键盘
2. 未签名版本首次运行时，可能被 Gatekeeper 拦截。
3. Retina / 多显示器场景仍建议实机验证坐标精度。

## 依赖环境

建议环境：

- macOS 12+
- Xcode Command Line Tools
- CMake 3.10+
- Qt 5.15.x
- FFmpeg
- OpenSSL

建议使用 Homebrew 安装依赖：

```bash
xcode-select --install
brew install cmake openssl@3 ffmpeg
brew install qt@5
```

如果你的机器已安装 Qt 官方包，也可以直接使用 Qt 安装目录，不强制要求 brew 安装 Qt。

## 配置构建

### 方式一：Qt 官方安装目录

```bash
cmake -S . -B out/build/macos \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/5.15.2/clang_64 \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
  -DFFMPEG_ROOT_DIR=/opt/homebrew/opt/ffmpeg
```

### 方式二：brew 安装 Qt5

Intel Mac 通常类似：

```bash
cmake -S . -B out/build/macos \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=$(brew --prefix qt@5) \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
  -DFFMPEG_ROOT_DIR=$(brew --prefix ffmpeg)
```

Apple Silicon 机器如果路径不同，请以本机 `brew --prefix` 输出为准。

## 编译

```bash
cmake --build out/build/macos --config Release -j$(sysctl -n hw.ncpu)
```

## 部署 .app

当前工程已在 CMake 中增加 `MACOSX_BUNDLE` 支持。

编译后可尝试执行：

```bash
cmake --build out/build/macos --target mac_bundle --config Release
```

如果本机存在 `macdeployqt`，会自动把 Qt 依赖部署进 `.app`。  
如果没有该工具，需要手动执行类似命令：

```bash
/path/to/Qt/5.15.2/clang_64/bin/macdeployqt out/build/macos/release/airan-desk.app
```

## 首次运行权限设置

此项目是远程桌面工具，macOS 上至少要处理以下权限：

### 1. 屏幕录制权限

进入：

- **系统设置 -> 隐私与安全性 -> 屏幕录制**

允许 `airan-desk.app`。

否则远程端可能出现：

- 黑屏
- 抓屏失败
- 只有窗口框架没有实际画面

### 2. 辅助功能权限

进入：

- **系统设置 -> 隐私与安全性 -> 辅助功能**

允许 `airan-desk.app`。

否则远程输入控制可能无效。

### 3. 输入监控（视系统版本而定）

部分系统版本或场景下，还可能需要：

- **系统设置 -> 隐私与安全性 -> 输入监控**

如果发现键盘事件不能稳定生效，可以检查此项。

## 未签名版本如何打开

如果你没有 Apple Developer 账号，仍可用于开发和内测。

首次打开若被拦截，可尝试：

1. Finder 中右键应用，选择 **打开**
2. 或前往：
   - **系统设置 -> 隐私与安全性 -> 仍要打开**

如果应用来自网络下载，某些机器还可能带有 quarantine 标记，可手动移除：

```bash
xattr -dr com.apple.quarantine /path/to/airan-desk.app
```

> 仅对自己信任的程序执行该命令。

## 运行说明

启动前请确认 `.app` 或可执行文件旁边已包含：

- `conf/`
- `locale/`

并正确配置：

```ini
[signal_server]
wsUrl=wss://your-signal-server.example/ws
```
## 排查建议

### 能启动但看不到桌面

优先检查：

- 是否已开启屏幕录制权限
- 是否重新启动应用

### 能看到画面但无法控制

优先检查：

- 是否已开启辅助功能权限
- 是否已开启输入监控

### 鼠标坐标不准

优先检查：

- 是否为 Retina 屏
- 是否使用了多显示器
- 主副屏排列是否改变

此类问题通常需要实机微调坐标映射。
