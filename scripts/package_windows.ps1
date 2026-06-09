$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
& "$root\scripts\build_windows.ps1"
$pkg = Join-Path $root "dist\TTrans-windows-portable.zip"
if (Test-Path $pkg) { Remove-Item $pkg }
Compress-Archive -Path (Join-Path $root "dist\ttrans.exe") -DestinationPath $pkg
Write-Host "Packaged $pkg"
