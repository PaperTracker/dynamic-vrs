@echo off
rem Registers the OpenXR API layer only. No file installed by Steam or SteamVR is touched.
rem Equivalent to running scripts\install.ps1 -SkipOpenVr, which also works on its own.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1" -SkipOpenVr
if errorlevel 1 pause
