param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "TTrans")
)

$ErrorActionPreference = "Stop"

$port = 44777
$rules = @(
    @{
        Name = "TTrans GUI UDP 44777"
        Program = Join-Path $InstallDir "ttrans-gui.exe"
    },
    @{
        Name = "TTrans CLI UDP 44777"
        Program = Join-Path $InstallDir "ttrans.exe"
    }
)

foreach ($rule in $rules) {
    if (-not (Test-Path $rule.Program)) {
        continue
    }
    Get-NetFirewallRule -DisplayName $rule.Name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule `
        -DisplayName $rule.Name `
        -Direction Inbound `
        -Action Allow `
        -Protocol UDP `
        -LocalPort $port `
        -Program $rule.Program `
        -Profile Any | Out-Null
}

Write-Host "TTrans firewall rules added for UDP $port."
