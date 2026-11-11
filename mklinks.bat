@echo off
cd /d "%~dp0"
@echo on
mklink /J rt-thread ..\..\rt-thread
mklink /J libraries ..\..\libraries
mklink /J libs ..\libs
mklink /J tools ..\..\tools
rem resources: Wi-Fi 固件/CLM/NVRAM，烧录资源用（不随固件下载）
mklink /J resources ..\..\resources