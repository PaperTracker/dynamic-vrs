[CmdletBinding()]
param([string]$InstallDirectory)
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "..\lib\openxr.ps1")

if (-not $InstallDirectory) { $InstallDirectory = Get-DvrOpenXrDirectory }

$manifest = Get-DvrOpenXrManifestPath -InstallDirectory $InstallDirectory
if (-not (Test-DvrOpenXrLayerRegistered -ManifestPath $manifest)) {
    Write-Output "The OpenXR layer is not registered. Settings are in $InstallDirectory"
    return
}
$registry = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Khronos\OpenXR\1\ApiLayers\Implicit', $true)
try {
    $registry.DeleteValue($manifest, $false)
} finally {
    $registry.Dispose()
}
Write-Output "OpenXR layer unregistered. Restart the OpenXR game to unload it. Settings are in $InstallDirectory"
