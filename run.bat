@echo off
rem Serve web\ on http://localhost:8771 (Web Serial needs a secure context) and open it.
cd /d "%~dp0"
if not exist web\index.wasm call build.bat || exit /b 1
start "" http://localhost:8771/
python -m http.server 8771 --directory web
