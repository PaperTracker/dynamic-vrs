[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = "High")]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallManifestPath,
    [switch]$RemoveHelperFiles,
    # With -RemoveHelperFiles: leave dynamic-vrs.ini in place, for a reinstall that keeps settings.
    [switch]$KeepConfig
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-Sha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Assert-ChildPath {
    param(
        [string]$Parent,
        [string]$Child
    )

    $resolvedParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $resolvedChild = [System.IO.Path]::GetFullPath($Child)
    $prefix = $resolvedParent + [System.IO.Path]::DirectorySeparatorChar
    if (-not $resolvedChild.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to operate outside the SteamVR bin directory: $resolvedChild"
    }
}

$resolvedManifest = [System.IO.Path]::GetFullPath($InstallManifestPath)
if (-not (Test-Path -LiteralPath $resolvedManifest -PathType Leaf)) {
    throw "Install manifest was not found: $resolvedManifest"
}
$manifest = Get-Content -LiteralPath $resolvedManifest -Raw | ConvertFrom-Json

$running = @(Get-Process -Name vrserver, vrcompositor, vrmonitor, vrwebhelper, vrstartup `
    -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw "SteamVR processes are running: $($running.ProcessName -join ', '). Stop SteamVR before restore."
}

$activeVrClient = [System.IO.Path]::GetFullPath([string]$manifest.activeVrClient)
$backupVrClient = [System.IO.Path]::GetFullPath([string]$manifest.backupVrClient)
foreach ($file in @($activeVrClient, $backupVrClient)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required restore file was not found: $file"
    }
}

$activeSha256 = Get-Sha256 -Path $activeVrClient
$proxySha256 = [string]$manifest.proxySha256
$originalSha256 = [string]$manifest.originalSha256
# Neither hash: SteamVR replaced the proxy after installation, usually in an update. That
# file is SteamVR's own and is left alone; only the inactive helpers can be cleaned up.
$steamVrReplacedProxy = $activeSha256 -ne $proxySha256 -and $activeSha256 -ne $originalSha256
if ((Get-Sha256 -Path $backupVrClient) -ne [string]$manifest.originalSha256) {
    throw "The backup vrclient hash is invalid. Refusing to restore it."
}

if ($PSCmdlet.ShouldProcess($activeVrClient, "Restore the original SteamVR vrclient_x64.dll")) {
    if ($activeSha256 -eq $proxySha256) {
        Copy-Item -LiteralPath $backupVrClient -Destination $activeVrClient -Force
    }
    if (-not $steamVrReplacedProxy -and (Get-Sha256 -Path $activeVrClient) -ne [string]$manifest.originalSha256) {
        throw "The restored vrclient hash does not match the recorded original."
    }

    $helperResult = "Helper files were retained but are inactive."
    if ($RemoveHelperFiles) {
        if ([int]$manifest.schemaVersion -lt 2) {
            throw "This install manifest predates verified helper cleanup. Restore completed, but helpers were retained."
        }

        $steamVrBin = Split-Path -Parent $activeVrClient
        $realVrClient = [System.IO.Path]::GetFullPath([string]$manifest.realVrClient)
        $liveConfig = [System.IO.Path]::GetFullPath([string]$manifest.liveConfig)
        $livePayload = [System.IO.Path]::GetFullPath([string]$manifest.livePayload)
        foreach ($helper in @($realVrClient, $liveConfig, $livePayload)) {
            Assert-ChildPath -Parent $steamVrBin -Child $helper
        }

        if ((Get-Sha256 -Path $realVrClient) -ne [string]$manifest.originalSha256) {
            throw "The inactive real_vrclient_x64.dll hash changed. Refusing helper cleanup."
        }
        # A config edited after installation (Save as defaults, or by hand) is moved next to
        # this manifest instead of being deleted.
        $configChanged = (Test-Path -LiteralPath $liveConfig -PathType Leaf) -and
            (Get-Sha256 -Path $liveConfig) -ne [string]$manifest.configSha256

        $expectedPayload = @{}
        foreach ($entry in @($manifest.payloadFiles)) {
            $relativePath = [string]$entry.relativePath
            if ([System.IO.Path]::IsPathRooted($relativePath) -or $relativePath.Contains("..")) {
                throw "Invalid relative payload path in install manifest: $relativePath"
            }
            $expectedPayload[$relativePath] = [string]$entry.sha256
        }
        $payloadPrefix = $livePayload.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
        $actualPayload = @(Get-ChildItem -LiteralPath $livePayload -File -Recurse)
        if ($actualPayload.Count -ne $expectedPayload.Count) {
            throw "The inactive payload file count changed. Refusing helper cleanup."
        }
        foreach ($file in $actualPayload) {
            $relativePath = $file.FullName.Substring($payloadPrefix.Length)
            if (-not $expectedPayload.ContainsKey($relativePath) -or
                (Get-Sha256 -Path $file.FullName) -ne $expectedPayload[$relativePath]) {
                throw "The inactive payload changed: $relativePath. Refusing helper cleanup."
            }
        }

        Remove-Item -LiteralPath $realVrClient -Force
        $realVrClientParent = Split-Path -Parent $realVrClient
        if ($realVrClientParent -ne $steamVrBin) {
            Assert-ChildPath -Parent $steamVrBin -Child $realVrClientParent
            if (@(Get-ChildItem -LiteralPath $realVrClientParent -Force).Count -eq 0) {
                Remove-Item -LiteralPath $realVrClientParent -Force
            }
        }
        Remove-Item -LiteralPath $livePayload -Recurse -Force
        $helperResult = "Verified helper files were removed."
        if ($KeepConfig) {
            $helperResult += " dynamic-vrs.ini was kept."
        } elseif ($configChanged) {
            $savedConfig = Join-Path (Split-Path -Parent $resolvedManifest) "dynamic-vrs.ini"
            Move-Item -LiteralPath $liveConfig -Destination $savedConfig
            $helperResult += " The edited dynamic-vrs.ini was moved to $savedConfig."
        } elseif (Test-Path -LiteralPath $liveConfig) {
            Remove-Item -LiteralPath $liveConfig -Force
        }
    }

    $marker = Join-Path (Split-Path -Parent $resolvedManifest) "restored.txt"
    [System.IO.File]::WriteAllText(
        $marker,
        ("Restored at UTC " + [DateTime]::UtcNow.ToString("o")),
        [System.Text.UTF8Encoding]::new($false))
    $restoreResult = if ($steamVrReplacedProxy) {
        "SteamVR had already replaced the layer proxy (usually after a SteamVR update); its vrclient was left as is."
    } elseif ($activeSha256 -eq $proxySha256) {
        "Original SteamVR vrclient restored."
    } else {
        "Original SteamVR vrclient was already restored and was verified again."
    }
    Write-Output "$restoreResult $helperResult"
}
