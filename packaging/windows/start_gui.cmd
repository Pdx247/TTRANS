@echo off
cd /d "%~dp0"
start "" "%~dp0ttrans-gui.exe" --port 44777 --out downloads
