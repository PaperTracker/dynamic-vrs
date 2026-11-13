@echo off
rem Removes the OpenVR and OpenXR layers and restores SteamVR. Exit Steam first.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\uninstall.ps1" -PauseAtEnd %*
