$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null

$gpp = "g++"
if (Test-Path "D:\MinGW\bin\g++.exe") {
    $gpp = "D:\MinGW\bin\g++.exe"
}

& $gpp -std=c++14 -O2 -Wall -Wextra -I "$root\include" `
    "$root\src\main.cpp" "$root\src\protocol.cpp" "$root\src\socket.cpp" "$root\src\transfer.cpp" "$root\src\web_gui.cpp" `
    -lws2_32 -o "$dist\ttrans.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Built $dist\ttrans.exe"
