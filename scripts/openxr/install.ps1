[CmdletBinding()]
param(
    [string]$BinDirectory,
    # Rendering mode written to a new dynamic-vrs.ini. An existing one is kept as it is.
    [ValidateSet("gaze", "fixed")]
    [string]$Mode = "gaze",
    [string]$InstallDirectory,
    [string]$SettingsPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "..\lib\openxr.ps1")

if (-not $InstallDirectory) { $InstallDirectory = Get-DvrOpenXrDirectory }
if (-not $BinDirectory) {
    $packagedBin = Join-Path $PSScriptRoot "..\..\bin"
    $BinDirectory = if (Test-Path -LiteralPath $packagedBin -PathType Container) {
        $packagedBin
    } else {
        Join-Path $PSScriptRoot "..\..\out\dynamic-vrs\build\Release"
    }
}
$InstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)
$BinDirectory = [IO.Path]::GetFullPath($BinDirectory)
$files = @("DynamicVRSLayer64.dll", "DynamicVRSPanel64.exe")
foreach ($name in $files) {
    $source = Join-Path $BinDirectory $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Build output missing: $source" }
    $destination = Join-Path $InstallDirectory $name
    if (Test-Path -LiteralPath $destination) {
        try {
            $stream = [IO.File]::Open($destination, 'Open', 'ReadWrite', 'None')
            $stream.Dispose()
        } catch {
            throw "Close the OpenXR game and its control panel before updating $destination"
        }
    }
}
$layerPath = Join-Path $BinDirectory "DynamicVRSLayer64.dll"
# Verify the exported loader negotiation entry with LoadLibraryEx's data-independent lookup below.
if (-not ('DvrOpenXrExports' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DvrOpenXrExports {
    [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32", CharSet=CharSet.Ansi)]
    public static extern IntPtr GetProcAddress(IntPtr module, string name);
    [DllImport("kernel32")]
    public static extern bool FreeLibrary(IntPtr module);
}
'@
}
$module = [DvrOpenXrExports]::LoadLibraryEx($layerPath, [IntPtr]::Zero, 1)
if ($module -eq [IntPtr]::Zero) { throw "Cannot inspect $layerPath" }
try {
    if ([DvrOpenXrExports]::GetProcAddress($module, "xrNegotiateLoaderApiLayerInterface") -eq [IntPtr]::Zero) {
        throw "This layer was built without OPENXR_SDK_ROOT. Rebuild with the OpenXR SDK."
    }
} finally {
    [void][DvrOpenXrExports]::FreeLibrary($module)
}
New-Item -ItemType Directory -Path $InstallDirectory -Force | Out-Null
foreach ($name in $files) { Copy-Item -LiteralPath (Join-Path $BinDirectory $name) -Destination $InstallDirectory -Force }
$config = Join-Path $InstallDirectory "dynamic-vrs.ini"
if (-not (Test-Path -LiteralPath $config)) {
    $source = if ($SettingsPath) { $SettingsPath } else { Join-Path $PSScriptRoot "..\..\config\dynamic-vrs.ini.example" }
    $text = Get-Content -LiteralPath $source -Raw
    $text = $text -replace '(?m)^enabled=.*$', 'enabled=1'
    $text = $text -replace '(?m)^nativeVrsMode=.*$', "nativeVrsMode=$Mode"
    [IO.File]::WriteAllText($config, $text, [Text.UTF8Encoding]::new($false))
}
$manifest = Join-Path $InstallDirectory "openxr-layer.json"
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "..\..\config\openxr-layer.json") -Destination $manifest -Force
$registry = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Khronos\OpenXR\1\ApiLayers\Implicit')
try {
    $registry.SetValue($manifest, 0, [Microsoft.Win32.RegistryValueKind]::DWord)
} finally {
    $registry.Dispose()
}
Write-Output "OpenXR D3D11 layer installed for the current user. Restart the OpenXR game to load it."
Write-Output "Settings: $config"
Write-Output "Manifest: $manifest"
