@echo off
cd /d "%~dp0"
start "" "http://127.0.0.1:47880"
ttrans.exe gui --http-port 47880
