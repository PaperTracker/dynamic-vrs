Set-StrictMode -Version Latest

# Helpers for the OpenXR integration.

. (Join-Path $PSScriptRoot "common.ps1")

# The per-user OpenXR layer: where its files go by default, and whether the implicit
# API-layer registration for them is present in HKCU.
function Get-DvrOpenXrDirectory {
    return (Join-Path (Get-DvrDataRoot) "openxr")
}

function Get-DvrOpenXrManifestPath {
    param([string]$InstallDirectory = (Get-DvrOpenXrDirectory))

    return Join-Path ([System.IO.Path]::GetFullPath($InstallDirectory)) "openxr-layer.json"
}

function Test-DvrOpenXrLayerRegistered {
    param([string]$ManifestPath = (Get-DvrOpenXrManifestPath))

    $registry = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Khronos\OpenXR\1\ApiLayers\Implicit')
    if (-not $registry) {
        return $false
    }
    try {
        return $null -ne $registry.GetValue($ManifestPath, $null)
    } finally {
        $registry.Dispose()
    }
}
