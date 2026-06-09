$ErrorActionPreference = "Stop"
param(
    [int]$Port = 44777,
    [string]$Out = "downloads"
)

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$data = Join-Path $root "docker-data"
New-Item -ItemType Directory -Force -Path $data | Out-Null

docker run --rm -it `
    -p "${Port}:${Port}/udp" `
    -v "${data}:/data" `
    ttrans:latest receive --port $Port --out $Out
