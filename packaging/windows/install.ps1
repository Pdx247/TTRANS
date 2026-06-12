$ErrorActionPreference = "Stop"

$source = Split-Path -Parent $MyInvocation.MyCommand.Path
$installDir = Join-Path $env:LOCALAPPDATA "TTrans"
$startMenuDir = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\TTrans"

New-Item -ItemType Directory -Force -Path $installDir | Out-Null
New-Item -ItemType Directory -Force -Path $startMenuDir | Out-Null

Copy-Item -Force -Path (Join-Path $source "ttrans.exe") -Destination (Join-Path $installDir "ttrans.exe")
Copy-Item -Force -Path (Join-Path $source "ttrans-gui.exe") -Destination (Join-Path $installDir "ttrans-gui.exe")
Copy-Item -Force -Path (Join-Path $source "*.dll") -Destination $installDir
New-Item -ItemType Directory -Force -Path (Join-Path $installDir "assets") | Out-Null
Copy-Item -Force -Path (Join-Path $source "assets\fa-*.ttf") -Destination (Join-Path $installDir "assets")
Copy-Item -Force -Path (Join-Path $source "assets\FONT-AWESOME-LICENSE.txt") -Destination (Join-Path $installDir "assets")
Copy-Item -Force -Path (Join-Path $source "start_gui.cmd") -Destination (Join-Path $installDir "start_gui.cmd")
Copy-Item -Force -Path (Join-Path $source "uninstall.cmd") -Destination (Join-Path $installDir "uninstall.cmd")
Copy-Item -Force -Path (Join-Path $source "uninstall.ps1") -Destination (Join-Path $installDir "uninstall.ps1")

$desktop = [Environment]::GetFolderPath("Desktop")
$shell = New-Object -ComObject WScript.Shell

$desktopShortcut = $shell.CreateShortcut((Join-Path $desktop "TTrans.lnk"))
$desktopShortcut.TargetPath = Join-Path $installDir "ttrans-gui.exe"
$desktopShortcut.Arguments = "--port 44777 --out downloads"
$desktopShortcut.WorkingDirectory = $installDir
$desktopShortcut.IconLocation = Join-Path $installDir "ttrans-gui.exe"
$desktopShortcut.Save()

$menuShortcut = $shell.CreateShortcut((Join-Path $startMenuDir "TTrans.lnk"))
$menuShortcut.TargetPath = Join-Path $installDir "ttrans-gui.exe"
$menuShortcut.Arguments = "--port 44777 --out downloads"
$menuShortcut.WorkingDirectory = $installDir
$menuShortcut.IconLocation = Join-Path $installDir "ttrans-gui.exe"
$menuShortcut.Save()

$uninstallShortcut = $shell.CreateShortcut((Join-Path $startMenuDir "Uninstall TTrans.lnk"))
$uninstallShortcut.TargetPath = Join-Path $installDir "uninstall.cmd"
$uninstallShortcut.WorkingDirectory = $installDir
$uninstallShortcut.Save()

Write-Host "TTrans installed to $installDir"
Write-Host "Desktop shortcut created: TTrans"
