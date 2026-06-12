$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dist = Join-Path $root "dist"
$build = Join-Path $root "build"
New-Item -ItemType Directory -Force -Path $dist | Out-Null

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake is required to build TTrans with Dear ImGui. Download the GitHub Release package if this machine is only running TTrans."
}

$gpp = Get-Command g++.exe -ErrorAction SilentlyContinue
if ($gpp) {
    $mingw = Split-Path -Parent (Split-Path -Parent $gpp.Source)
    $env:Path = "$mingw\bin;$env:Path"
    $env:CMAKE_PREFIX_PATH = $mingw
}

cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$env:CMAKE_PREFIX_PATH"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Copy-Item -Force -Path (Join-Path $build "ttrans.exe") -Destination (Join-Path $dist "ttrans.exe")
Copy-Item -Force -Path (Join-Path $build "ttrans-gui.exe") -Destination (Join-Path $dist "ttrans-gui.exe")
New-Item -ItemType Directory -Force -Path (Join-Path $dist "assets") | Out-Null
Copy-Item -Force -Path (Join-Path $root "assets\fa-*.ttf") -Destination (Join-Path $dist "assets")
Copy-Item -Force -Path (Join-Path $root "assets\FONT-AWESOME-LICENSE.txt") -Destination (Join-Path $dist "assets")
Write-Host "Built $dist\ttrans.exe"
