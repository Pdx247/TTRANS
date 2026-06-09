# TTrans

TTrans 是一款轻量级局域网 UDP 文件传输工具，用于在 Windows、macOS 和 Linux 之间交换文件。核心用 C++14 实现，同时提供 CLI 和本地 Web GUI。GUI 模式会启动一个只监听 `127.0.0.1` 的小面板，支持图片/文本文件预览，再通过 C++ 后端进行 UDP 分块传输。

## 功能

- UDP 分块传输，固定头部协议，分块 ACK 与超时重传
- CLI：发送和接收文件
- GUI：本地 Web 小界面，支持文件预览、发送、接收、实时日志
- 跨平台源码：Windows、Linux、macOS
- 打包脚本：Windows `.exe`/portable zip、Linux ELF tarball、macOS `.dmg`

## 给普通 Windows 用户

对方电脑不需要安装 g++，也不需要 Docker。让对方下载 `TTrans-windows-installer.zip`，解压后双击 `install.cmd`，桌面会生成 `TTrans` 快捷方式。

```powershell
.\scripts\package_windows.ps1
```

生成的安装包在 `dist\TTrans-windows-installer.zip`。

## 构建

Windows:

```powershell
.\scripts\build_windows.ps1
.\dist\ttrans.exe --help
```

Linux:

```sh
sh scripts/build_linux.sh
./dist/ttrans --help
```

macOS:

```sh
sh scripts/package_macos.sh
```

## 使用

接收端:

```sh
ttrans receive --port 44777 --out downloads
```

发送端:

```sh
ttrans send --host 192.168.1.23 --port 44777 --file ./demo.zip
```

GUI:

```sh
ttrans gui --http-port 47880
```

然后打开 `http://127.0.0.1:47880`。

## Docker 使用

如果电脑已经安装 Docker Desktop，也可以用 Docker 运行。

构建镜像:

```powershell
.\scripts\docker_build.ps1
```

启动 GUI:

```powershell
.\scripts\docker_gui.ps1
```

CLI 接收:

```powershell
.\scripts\docker_receive.ps1 -Port 44777 -Out downloads
```

CLI 发送:

```powershell
.\scripts\docker_send.ps1 -HostIp 192.168.1.23 -Port 44777 -File .\demo.zip
```

也可以用 Compose:

```powershell
docker compose up --build
```

Docker 运行时会映射 `44777/udp` 和 `47880/tcp`，并把容器内 `/data` 映射到本地 `docker-data`。

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

当前机器是 Windows，且没有 macOS/Linux 交叉工具链，所以 `.dmg` 和 Linux ELF 需要分别在 macOS/Linux 上运行对应脚本生成。
