$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
& "$root\scripts\build_windows.ps1"
$packageDir = Join-Path $root "dist\TTrans-windows"
$pkg = Join-Path $root "dist\TTrans-windows-installer.zip"
if (Test-Path $packageDir) { Remove-Item -Recurse -Force $packageDir }
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null
Copy-Item -Force -Path (Join-Path $root "dist\ttrans.exe") -Destination (Join-Path $packageDir "ttrans.exe")
$sdl = where.exe SDL2.dll 2>$null | Select-Object -First 1
if ($sdl) {
    Copy-Item -Force -Path $sdl -Destination (Join-Path $packageDir "SDL2.dll")
}
Copy-Item -Force -Path (Join-Path $root "packaging\windows\*") -Destination $packageDir
if (Test-Path $pkg) { Remove-Item $pkg }
Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $pkg
Write-Host "Packaged $pkg"
