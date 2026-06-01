@echo off
path=D:\Mysoftware\Qt\6.11.0\msvc2022_64\bin;D:\halcon24.11\HALCON-24.11-Progress-Steady\bin\x64-win64;D:\open\opencv\build\x64\vc16\bin;%PATH%
cd /d "%~dp0build\Release"
RobotVisionSystem.exe
