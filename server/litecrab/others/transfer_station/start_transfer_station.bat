@echo off
chcp 65001 >nul
setlocal
set PYTHON=C:\Users\zhangshuaishuai\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe
set SCRIPT_DIR=%~dp0
set PYTHONPATH=%SCRIPT_DIR%..
"%PYTHON%" -m transfer_station.main --config "%SCRIPT_DIR%config.json" %*
echo.
echo [transfer station exited, press any key to close]
pause >nul
