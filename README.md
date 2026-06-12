# TTrans

TTrans 是一款轻量级局域网 UDP 文件传输工具，用于在 Windows、macOS 和 Linux 之间交换文件。核心传输协议用 C++14 实现，界面使用 Dear ImGui 原生 GUI，同时保留 CLI。

## 给普通 Windows 用户

对方电脑不需要安装 g++，也不需要 Docker。让对方下载 `TTrans-windows-installer.zip`，解压后双击 `install.cmd`，桌面会生成 `TTrans` 快捷方式。

Windows 安装时会尝试添加防火墙规则，放行 `44777/udp`。如果系统弹出管理员权限或防火墙确认，请允许；否则同一局域网内也可能搜不到对方。

下载页:

```text
https://github.com/Pdx247/TTRANS/releases
```

本地生成安装包:

```powershell
.\scripts\package_windows.ps1
```

生成的安装包在 `dist\TTrans-windows-installer.zip`。

## 功能

- UDP 分块传输，固定头部协议，分块 ACK 与超时重传
- CLI：发送和接收文件
- Dear ImGui 原生 GUI：发送、持续监听接收端口、收到文件时弹窗接受/拒绝、日志、文本预览
- Windows 桌面快捷方式启动无控制台的 `ttrans-gui.exe`
- Windows 包内置程序图标和运行所需 DLL
- 发送和接收都保留原始文件名
- Windows 下支持中文文件路径和中文文件名
- 跨平台源码：Windows、Linux、macOS
- 自动发布包：Windows installer zip、Linux ELF tarball、macOS dmg

## GUI 使用

启动 GUI:

```sh
ttrans-gui --port 44777 --out downloads
```

GUI 一打开就会持续监听 `44777/udp`。收到文件时会弹出确认窗口，点击 `Accept` 才会接收，点击 `Reject` 会通知发送方取消。

发送文件时把文件拖到窗口里的发送区，或者点击 `Choose File` 打开系统文件选择器；发送和接收都会保留文件本身的原始文件名。自动搜索失败时，可以在左侧输入对方 IPv4 地址并点击 `Use`。

如果 `Search` 搜不到同一 Wi-Fi 的另一台电脑，先确认两端都是新版并允许了 `44777/udp`。部分学校、酒店、公司 WLAN 会开启客户端隔离，哪怕 IP 看起来在同一网段，也会阻止设备互相发现和传输。

## CLI 使用

接收端:

```sh
ttrans receive --port 44777 --out downloads
```

发送端:

```sh
ttrans send --host 192.168.1.23 --port 44777 --file ./demo.zip
```

## 构建

Windows 需要 CMake、MinGW、SDL2。推荐直接使用 GitHub Release 里的安装包。

Windows:

```powershell
.\scripts\build_windows.ps1
.\dist\ttrans.exe --help
```

Linux:

```sh
sudo apt-get install libsdl2-dev
sh scripts/build_linux.sh
./dist/ttrans --help
```

macOS:

```sh
brew install sdl2
sh scripts/package_macos.sh
```

## 打包

Windows:

```powershell
.\scripts\package_windows.ps1
```

Linux:

```sh
sh scripts/package_linux.sh
```

macOS:

```sh
sh scripts/package_macos.sh
```
