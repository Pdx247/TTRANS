@echo off
cd /d "%~dp0"
start "" "%~dp0ttrans.exe" gui --port 44777 --out downloads
