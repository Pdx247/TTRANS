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
Copy-Item -Force -Path (Join-Path $source "firewall.ps1") -Destination (Join-Path $installDir "firewall.ps1")

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

try {
    $firewallScript = Join-Path $installDir "firewall.ps1"
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    $isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) {
        & $firewallScript -InstallDir $installDir
    } else {
        Write-Host "Requesting Windows Firewall permission for UDP 44777..."
        Start-Process powershell `
            -Verb RunAs `
            -Wait `
            -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$firewallScript`"", "-InstallDir", "`"$installDir`"")
    }
} catch {
    Write-Host "Firewall rule was not added. If peer search fails, allow TTrans on Private networks or open UDP 44777."
}

Write-Host "TTrans installed to $installDir"
Write-Host "Desktop shortcut created: TTrans"
