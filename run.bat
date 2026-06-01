@echo off
rem ── 环境检查 ────────────────────────────────────────────
if "%QT6_DIR%"=="" (
    echo [错误] 未设置 QT6_DIR，请指向 Qt 安装路径（如 D:\Qt\6.11.0\msvc2022_64）
    pause
    exit /b 1
)
if "%HALCONROOT%"=="" (
    echo [错误] 未设置 HALCONROOT，请指向 HALCON 安装路径（如 C:\Program Files\MVTec\HALCON-24.11）
    pause
    exit /b 1
)
if "%OpenCV_DIR%"=="" (
    echo [错误] 未设置 OpenCV_DIR，请指向 OpenCV 构建路径（如 C:\opencv\build）
    pause
    exit /b 1
)

rem ── 设置 PATH ────────────────────────────────────────────
path=%QT6_DIR%\bin;%HALCONROOT%\bin\x64-win64;%OpenCV_DIR%\x64\vc16\bin;%PATH%

rem ── 启动 ─────────────────────────────────────────────────
cd /d "%~dp0build\Release"
RobotVisionSystem.exe
