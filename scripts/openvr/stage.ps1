[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$SteamVrRoot,
    # Where to build the staged layout; a new timestamped folder under out\ by default.
    [string]$OutputDirectory,
    # Directory holding the built binaries; defaults to the build.ps1 output.
    [string]$BuildDirectory,
    # Rendering mode written to the staged dynamic-vrs.ini. Every other setting keeps its template default.
    [ValidateSet("off", "gaze", "fixed")]
    [string]$Mode = "off",
    # Keep SteamVR's client under its own file name (dynamic_vrs_real\vrclient_x64.dll) and have the proxy
    # report its own path as that file's, for launchers that look the DLL up by name.
    [switch]$PreserveRealVrClientBaseName
)

# Builds the layout install.ps1 installs without touching SteamVR: the proxy and SteamVR's own client
# side by side, the layer and panel in the payload folder, a dynamic-vrs.ini made from the template, and a
# manifest with the SHA-256 of each file.

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "..\lib\steamvr.ps1")

function Get-FileRecord {
    param([Parameter(Mandatory = $true)][string]$Path)

    $item = Get-Item -LiteralPath $Path
    $hash = Get-FileHash -LiteralPath $Path -Algorithm SHA256
    return [PSCustomObject]@{
        path = $item.FullName
        length = $item.Length
        sha256 = $hash.Hash
        lastWriteTimeUtc = $item.LastWriteTimeUtc.ToString("o")
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$buildDirectory = if ($BuildDirectory) {
    [System.IO.Path]::GetFullPath($BuildDirectory)
} else {
    Join-Path $repositoryRoot "out\dynamic-vrs\build\$Configuration"
}
$proxySource = Join-Path $buildDirectory "vrclient_x64.dll"
$probeSource = Join-Path $buildDirectory "dynamic_vrs_proxy_probe.exe"
$nativeLayerSource = Join-Path $buildDirectory "DynamicVRSLayer64.dll"
# The layer launches the control panel from its own directory.
$controlPanelSources = @("DynamicVRSPanel64.exe") | ForEach-Object { Join-Path $buildDirectory $_ }
$configTemplate = Join-Path $repositoryRoot "config\dynamic-vrs.ini.example"

foreach ($file in @($proxySource, $probeSource, $nativeLayerSource, $configTemplate) + $controlPanelSources) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required build artifact not found: $file. Run build.ps1 first."
    }
}

$resolvedSteamVrRoot = Resolve-DvrSteamVrRoot -RequestedRoot $SteamVrRoot
$realVrClientSource = Join-Path $resolvedSteamVrRoot "bin\vrclient_x64.dll"
if (-not (Test-Path -LiteralPath $realVrClientSource -PathType Leaf)) {
    throw "SteamVR vrclient_x64.dll not found: $realVrClientSource"
}

if (-not $OutputDirectory) {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss-fff"
    $OutputDirectory = Join-Path $repositoryRoot "out\dynamic-vrs\staging\$timestamp"
}
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $resolvedOutput) {
    throw "Staging directory already exists; choose a new directory: $resolvedOutput"
}

New-Item -ItemType Directory -Path $resolvedOutput | Out-Null
$realVrClientRelativePath = if ($PreserveRealVrClientBaseName) {
    "dynamic_vrs_real\vrclient_x64.dll"
} else {
    "real_vrclient_x64.dll"
}
$stagedRealVrClient = Join-Path $resolvedOutput $realVrClientRelativePath
New-Item -ItemType Directory -Path (Split-Path -Parent $stagedRealVrClient) -Force | Out-Null
Copy-Item -LiteralPath $proxySource -Destination (Join-Path $resolvedOutput "vrclient_x64.dll")
Copy-Item -LiteralPath $realVrClientSource -Destination $stagedRealVrClient
Copy-Item -LiteralPath $probeSource -Destination (Join-Path $resolvedOutput "dynamic_vrs_proxy_probe.exe")

$payloadOutput = Join-Path $resolvedOutput "dynamic-vrs_payload"
New-Item -ItemType Directory -Path $payloadOutput -Force | Out-Null
Copy-Item -LiteralPath $nativeLayerSource -Destination (Join-Path $payloadOutput "DynamicVRSLayer64.dll")
foreach ($file in $controlPanelSources) {
    Copy-Item -LiteralPath $file -Destination $payloadOutput
}

# The template ends each line with "`r`n" when checked out with CRLF, so the "(?m)^key=value$" anchors below
# would never match and the staged ini would silently keep the template defaults (enabled=0, nativeVrsMode=off).
$configText = (Get-Content -LiteralPath $configTemplate -Raw) -replace "`r`n", "`n"
$configText = $configText -replace "(?m)^enabled=0$", "enabled=1"
$configText = $configText -replace "(?m)^nativeVrsMode=off$", "nativeVrsMode=$Mode"
$configText = $configText -replace "(?m)^realVrClientPath=.*$", "realVrClientPath=$realVrClientRelativePath"
$configText = $configText -replace "(?m)^spoofRealVrClientPath=0$", ("spoofRealVrClientPath=" + $(if ($PreserveRealVrClientBaseName) { "1" } else { "0" }))
[System.IO.File]::WriteAllText(
    (Join-Path $resolvedOutput "dynamic-vrs.ini"),
    $configText,
    [System.Text.UTF8Encoding]::new($false))

$proxyExports = Get-DvrPeExportNames -Path (Join-Path $resolvedOutput "vrclient_x64.dll")
$realExports = Get-DvrPeExportNames -Path $stagedRealVrClient
[System.IO.File]::WriteAllLines((Join-Path $resolvedOutput "proxy_exports.txt"), [string[]]$proxyExports)
[System.IO.File]::WriteAllLines((Join-Path $resolvedOutput "real_exports.txt"), [string[]]$realExports)
foreach ($exportName in @('HmdSystemFactory', 'VRClientCoreFactory', 'xrNegotiateLoaderRuntimeInterface')) {
    if ($proxyExports -notcontains $exportName -or $realExports -notcontains $exportName) {
        throw "Required OpenVR export missing from staged proxy or real client: $exportName"
    }
}

$manifest = [PSCustomObject]@{
    schemaVersion = 4
    createdAtUtc = [DateTime]::UtcNow.ToString("o")
    nativeVrsMode = $Mode
    preserveRealVrClientBaseName = [bool]$PreserveRealVrClientBaseName
    realVrClientRelativePath = $realVrClientRelativePath
    steamVrRoot = $resolvedSteamVrRoot
    files = [PSCustomObject]@{
        proxy = Get-FileRecord -Path (Join-Path $resolvedOutput "vrclient_x64.dll")
        realVrClient = Get-FileRecord -Path $stagedRealVrClient
        config = Get-FileRecord -Path (Join-Path $resolvedOutput "dynamic-vrs.ini")
        layer = Get-FileRecord -Path (Join-Path $payloadOutput "DynamicVRSLayer64.dll")
    }
}
[System.IO.File]::WriteAllText(
    (Join-Path $resolvedOutput "staging_manifest.json"),
    ($manifest | ConvertTo-Json -Depth 8),
    [System.Text.UTF8Encoding]::new($false))

Write-Output "Runtime staged without modifying SteamVR:"
Write-Output "  $resolvedOutput"
Write-Output "VRS mode: $Mode"
