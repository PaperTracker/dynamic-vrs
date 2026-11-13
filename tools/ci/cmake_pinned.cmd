@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "DYNAMIC_VRS_REPOSITORY_ROOT=%~dp0..\.."
set "DYNAMIC_VRS_MSVC_VERSION="
set "DYNAMIC_VRS_VCVARS="

set /p DYNAMIC_VRS_MSVC_VERSION=<"%DYNAMIC_VRS_REPOSITORY_ROOT%\cmake\pinned_msvc_toolset.txt"
for /f "usebackq delims=" %%I in (`powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; . '%~dp0msvc_toolset.ps1'; (Resolve-DvrMsvcToolset -RepositoryRoot '%DYNAMIC_VRS_REPOSITORY_ROOT%').VcVarsPath"`) do set "DYNAMIC_VRS_VCVARS=%%I"

if not defined DYNAMIC_VRS_MSVC_VERSION (
    echo Pinned MSVC toolset version could not be read. 1>&2
    exit /b 1
)
if not defined DYNAMIC_VRS_VCVARS (
    echo MSVC toolset %DYNAMIC_VRS_MSVC_VERSION% could not be resolved. 1>&2
    exit /b 1
)

call "%DYNAMIC_VRS_VCVARS%" -vcvars_ver=%DYNAMIC_VRS_MSVC_VERSION% >nul 2>&1
if errorlevel 1 (
    echo Failed to activate MSVC toolset %DYNAMIC_VRS_MSVC_VERSION%. 1>&2
    exit /b 1
)

set VSLANG=
cmake.exe %*
set "DYNAMIC_VRS_CMAKE_EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %DYNAMIC_VRS_CMAKE_EXIT_CODE%
