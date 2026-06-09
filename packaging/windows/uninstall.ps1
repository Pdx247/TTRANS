$ErrorActionPreference = "SilentlyContinue"

$installDir = Join-Path $env:LOCALAPPDATA "TTrans"
$startMenuDir = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\TTrans"
$desktopShortcut = Join-Path ([Environment]::GetFolderPath("Desktop")) "TTrans.lnk"

Remove-Item -Force -Path $desktopShortcut
Remove-Item -Recurse -Force -Path $startMenuDir
Remove-Item -Recurse -Force -Path $installDir

Write-Host "TTrans removed."
