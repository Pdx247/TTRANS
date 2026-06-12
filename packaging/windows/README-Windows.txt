TTrans Windows 安装包

安装:
1. 解压这个 zip。
2. 双击 install.cmd。
3. 如果 Windows 询问管理员权限，这是为了添加 UDP 44777 防火墙规则。
4. 安装完成后，双击桌面上的 TTrans 快捷方式。

如果 Windows 防火墙询问是否允许网络访问，请选择允许专用网络。

使用:
- 双击桌面 TTrans 快捷方式会打开原生 Dear ImGui 窗口
- 快捷方式启动的是无控制台版 ttrans-gui.exe，不会弹出黑色终端
- 默认 UDP 端口是 44777
- Search 会通过 UDP 44777 搜索同网段设备
- 自动搜索失败时，可以在左侧输入对方 IPv4 地址并点击 Use
- 接收文件默认保存到程序运行目录下的 downloads 文件夹
- 发送文件时把文件拖到窗口里的发送区，或点击 Choose File 选择文件
- 支持中文文件路径和中文文件名

如果同一个 Wi-Fi 下仍然搜不到，请确认两台电脑都安装新版，并且当前网络不是学校/公司常见的“客户端隔离”网络；这种网络会阻止同一 WLAN 内的电脑互相通信。

卸载:
- 开始菜单 -> TTrans -> Uninstall TTrans
  或进入安装目录双击 uninstall.cmd
