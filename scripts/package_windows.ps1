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
$gpp = Get-Command g++.exe -ErrorAction SilentlyContinue
if ($gpp) {
    $bin = Split-Path -Parent $gpp.Source
    foreach ($dll in @("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")) {
        $path = Join-Path $bin $dll
        if (Test-Path $path) {
            Copy-Item -Force -Path $path -Destination (Join-Path $packageDir $dll)
        }
    }
}
Copy-Item -Force -Path (Join-Path $root "packaging\windows\*") -Destination $packageDir
& (Join-Path $packageDir "ttrans.exe") --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Packaged ttrans.exe failed to start." }
if (Test-Path $pkg) { Remove-Item $pkg }
Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $pkg
Write-Host "Packaged $pkg"
