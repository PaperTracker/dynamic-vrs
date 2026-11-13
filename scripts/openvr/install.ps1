[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = "High")]
param(
    [Parameter(Mandatory = $true)]
    [string]$StageDirectory,
    [string]$SteamVrRoot,
    [string]$BackupRoot,
    # Keep a dynamic-vrs.ini already in SteamVR\bin (left by restore.ps1 -KeepConfig)
    # instead of refusing to install over it.
    [switch]$KeepExistingConfig
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "..\lib\steamvr.ps1")

if (-not $BackupRoot) { $BackupRoot = Get-DvrDefaultBackupRoot }

function Assert-SteamVrStopped {
    $running = @(Get-Process -Name vrserver, vrcompositor, vrmonitor, vrwebhelper, vrstartup `
        -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw "SteamVR processes are running: $($running.ProcessName -join ', '). Stop SteamVR before installation."
    }
}

function Get-Sha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

$resolvedStage = [System.IO.Path]::GetFullPath($StageDirectory)
$stageManifestPath = Join-Path $resolvedStage "staging_manifest.json"
if (-not (Test-Path -LiteralPath $stageManifestPath -PathType Leaf)) {
    throw "Staging manifest not found: $stageManifestPath"
}
$stageManifest = Get-Content -LiteralPath $stageManifestPath -Raw | ConvertFrom-Json

$resolvedSteamVrRoot = if ($SteamVrRoot) {
    [System.IO.Path]::GetFullPath($SteamVrRoot)
} else {
    [System.IO.Path]::GetFullPath([string]$stageManifest.steamVrRoot)
}
$steamVrBin = [System.IO.Path]::GetFullPath((Join-Path $resolvedSteamVrRoot "bin"))
$activeVrClient = Join-Path $steamVrBin "vrclient_x64.dll"
$realVrClientRelativePath = if ($stageManifest.PSObject.Properties['realVrClientRelativePath']) {
    [string]$stageManifest.realVrClientRelativePath
} else {
    "real_vrclient_x64.dll"
}
if ([System.IO.Path]::IsPathRooted($realVrClientRelativePath) -or
    $realVrClientRelativePath.Contains("..")) {
    throw "Invalid staged real vrclient relative path: $realVrClientRelativePath"
}
$realVrClient = [System.IO.Path]::GetFullPath((Join-Path $steamVrBin $realVrClientRelativePath))
$realVrClientParent = Split-Path -Parent $realVrClient
$liveConfig = Join-Path $steamVrBin "dynamic-vrs.ini"
$livePayload = Join-Path $steamVrBin "dynamic-vrs_payload"

if (-not (Test-Path -LiteralPath $activeVrClient -PathType Leaf)) {
    throw "Active SteamVR vrclient was not found: $activeVrClient"
}
if (Test-Path -LiteralPath $realVrClient) {
    throw "Refusing to overwrite an existing real vrclient: $realVrClient"
}
if ($realVrClientParent -ne $steamVrBin -and (Test-Path -LiteralPath $realVrClientParent)) {
    throw "Refusing to reuse an existing real vrclient directory: $realVrClientParent"
}
if (-not $KeepExistingConfig -and (Test-Path -LiteralPath $liveConfig)) {
    throw "Refusing to overwrite an existing config: $liveConfig"
}
if (Test-Path -LiteralPath $livePayload) {
    throw "Refusing to overwrite an existing payload directory: $livePayload"
}

$stagedOriginalHash = [string]$stageManifest.files.realVrClient.sha256
$activeOriginalHash = Get-Sha256 -Path $activeVrClient
if ($stagedOriginalHash -ne $activeOriginalHash) {
    throw "SteamVR vrclient changed after staging. Stage again before installation."
}

Assert-SteamVrStopped

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss-fff"
$resolvedBackupRoot = [System.IO.Path]::GetFullPath($BackupRoot)
$backupDirectory = Join-Path $resolvedBackupRoot $timestamp
$proxySource = Join-Path $resolvedStage "vrclient_x64.dll"
$realVrClientSource = Join-Path $resolvedStage $realVrClientRelativePath
$configSource = Join-Path $resolvedStage "dynamic-vrs.ini"
$payloadSource = Join-Path $resolvedStage "dynamic-vrs_payload"

foreach ($file in @($proxySource, $realVrClientSource, $configSource)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required staged file was not found: $file"
    }
}

$payloadManifest = @()
if (Test-Path -LiteralPath $payloadSource -PathType Container) {
    $payloadPrefix = [System.IO.Path]::GetFullPath($payloadSource).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $payloadManifest = @(
        Get-ChildItem -LiteralPath $payloadSource -File -Recurse | ForEach-Object {
            if (-not $_.FullName.StartsWith($payloadPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Payload file is outside the staged payload directory: $($_.FullName)"
            }
            [PSCustomObject]@{
                relativePath = $_.FullName.Substring($payloadPrefix.Length)
                sha256 = Get-Sha256 -Path $_.FullName
            }
        }
    )
}

if ($PSCmdlet.ShouldProcess($steamVrBin, "Install the DynamicVRS layer proxy")) {
    New-Item -ItemType Directory -Path $backupDirectory | Out-Null
    $backupVrClient = Join-Path $backupDirectory "vrclient_x64.dll"
    Copy-Item -LiteralPath $activeVrClient -Destination $backupVrClient

    # Install helpers first. The original SteamVR DLL remains active if any helper copy fails.
    New-Item -ItemType Directory -Path $realVrClientParent -Force | Out-Null
    Copy-Item -LiteralPath $realVrClientSource -Destination $realVrClient
    if (Test-Path -LiteralPath $liveConfig) {
        # A kept config may come from a different layout (another real vrclient path, for one).
        Sync-DvrConfigLayout -LiveConfig $liveConfig -StagedConfig $configSource
    } else {
        Copy-Item -LiteralPath $configSource -Destination $liveConfig
    }
    if (Test-Path -LiteralPath $payloadSource -PathType Container) {
        Copy-Item -LiteralPath $payloadSource -Destination $livePayload -Recurse
    }

    # Replace the active DLL last.
    Copy-Item -LiteralPath $proxySource -Destination $activeVrClient -Force

    $installedProxyHash = Get-Sha256 -Path $activeVrClient
    $expectedProxyHash = [string]$stageManifest.files.proxy.sha256
    if ($installedProxyHash -ne $expectedProxyHash) {
        Copy-Item -LiteralPath $backupVrClient -Destination $activeVrClient -Force
        throw "Installed proxy hash mismatch. The original vrclient was restored."
    }

    $installManifest = [PSCustomObject]@{
        schemaVersion = 3
        installedAtUtc = [DateTime]::UtcNow.ToString("o")
        steamVrRoot = $resolvedSteamVrRoot
        activeVrClient = $activeVrClient
        realVrClient = $realVrClient
        realVrClientRelativePath = $realVrClientRelativePath
        liveConfig = $liveConfig
        livePayload = $livePayload
        backupVrClient = $backupVrClient
        originalSha256 = $activeOriginalHash
        proxySha256 = $installedProxyHash
        configSha256 = Get-Sha256 -Path $configSource
        payloadFiles = $payloadManifest
    }
    $installManifestPath = Join-Path $backupDirectory "install_manifest.json"
    [System.IO.File]::WriteAllText(
        $installManifestPath,
        ($installManifest | ConvertTo-Json -Depth 5),
        [System.Text.UTF8Encoding]::new($false))

    Write-Output "Layer proxy installed. Restore manifest:"
    Write-Output "  $installManifestPath"
}
