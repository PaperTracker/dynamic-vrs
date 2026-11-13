@echo off
rem Installs the OpenVR and OpenXR layers. Exit Steam first.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1" -PauseAtEnd %*
