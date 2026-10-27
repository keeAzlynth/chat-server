@echo off
REM IM Chat Server startup script
REM Double-click to start the server

cd /d "%~dp0"

echo ==============================================
echo   IM Chat Server
echo   WebSocket: ws://127.0.0.1:8080
echo   Web 页面 : http://127.0.0.1:8081/
echo ==============================================
echo.

REM 找 exe 文件：优先级 Debug > 根目录 > Release
if exist "build-Debug\im_chat_server.exe" (
    echo 启动 Debug 构建...
    .\build-Debug\im_chat_server.exe .\config\config.json
) else if exist "im_chat_server.exe" (
    echo 启动中...
    .\im_chat_server.exe .\config\config.json
) else if exist "build-Release\im_chat_server.exe" (
    echo 启动 Release 构建...
    .\build-Release\im_chat_server.exe .\config\config.json
) else (
    echo ==============================================
    echo   [错误] 找不到 im_chat_server.exe
    echo.
    echo   请先在 PowerShell 中执行：
    echo   .\build.ps1 backend Debug
    echo ==============================================
)

pause
