$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$data = Join-Path $root "docker-data"
New-Item -ItemType Directory -Force -Path $data | Out-Null

docker run --rm -it `
    -p 127.0.0.1:47880:47880/tcp `
    -p 44777:44777/udp `
    -e TTRANS_HTTP_BIND=0.0.0.0 `
    -v "${data}:/data" `
    ttrans:latest gui --http-port 47880
