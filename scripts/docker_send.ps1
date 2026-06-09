$ErrorActionPreference = "Stop"
param(
    [Parameter(Mandatory=$true)][string]$HostIp,
    [int]$Port = 44777,
    [Parameter(Mandatory=$true)][string]$File
)

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$resolved = Resolve-Path -LiteralPath $File
$dir = Split-Path -Parent $resolved
$name = Split-Path -Leaf $resolved

docker run --rm -it `
    -v "${dir}:/send:ro" `
    ttrans:latest send --host $HostIp --port $Port --file "/send/$name"
