[CmdletBinding()]
param(
    [string]$SteamVrRoot,
    # Remove only one integration. By default both OpenVR and OpenXR are removed.
    [switch]$SkipOpenVr,
    [switch]$SkipOpenXr,
    # Uninstall without asking for confirmation.
    [switch]$Yes,
    # Wait for Enter before exiting, so a double-clicked window stays readable.
    [switch]$PauseAtEnd
)

# One-step uninstall for end users. OpenVR: restores SteamVR's own vrclient_x64.dll from
# every installation recorded in the backup directory and removes the layer files. A settings
# file edited since installation is moved to that installation's backup directory. Layer files
# without an install record (a manual install, for example) are backed up and removed too.
# OpenXR: unregisters the API layer; its files and settings stay in LOCALAPPDATA. The two
# parts are independent; one that cannot run does not stop the other.

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "lib\steamvr.ps1")
. (Join-Path $PSScriptRoot "lib\openxr.ps1")

# Puts SteamVR back to stock. Runs elevated when SteamVR\bin needs it.
function Uninstall-DvrOpenVr {
    param([Parameter(Mandatory = $true)][string]$SteamVrRoot)

    Assert-DvrVrClientReplaceable -SteamVrBin (Join-Path $SteamVrRoot "bin")
    foreach ($manifest in @(Get-DvrActiveInstallManifests)) {
        & (Join-Path $PSScriptRoot "openvr\restore.ps1") -InstallManifestPath $manifest `
            -RemoveHelperFiles -Confirm:$false
    }
    $unrecorded = Get-DvrUnrecordedInstall -SteamVrRoot $SteamVrRoot
    if ($unrecorded) {
        Remove-DvrUnrecordedInstall -Install $unrecorded
        # Remove-DvrUnrecordedInstall keeps a copy of the config in its backup directory.
        if (Test-Path -LiteralPath $unrecorded.Config -PathType Leaf) {
            Remove-Item -LiteralPath $unrecorded.Config -Force
        }
    }
}

$exitCode = 0
$removed = [System.Collections.Generic.List[string]]::new()
$failures = [System.Collections.Generic.List[string]]::new()
try {
    if ($SkipOpenVr -and $SkipOpenXr) {
        throw "Nothing to uninstall: both -SkipOpenVr and -SkipOpenXr were given."
    }
    $removeOpenVr = -not $SkipOpenVr
    $removeOpenXr = -not $SkipOpenXr

    $resolvedSteamVrRoot = $null
    $steamVrBin = $null
    if ($removeOpenVr) {
        $manifests = @(Get-DvrActiveInstallManifests)
        try {
            $resolvedSteamVrRoot = if ($SteamVrRoot -or $manifests.Count -eq 0) {
                Resolve-DvrSteamVrRoot -RequestedRoot $SteamVrRoot
            } else {
                $activeVrClient = [string](Get-Content -LiteralPath $manifests[0] -Raw | ConvertFrom-Json).activeVrClient
                Split-Path -Parent (Split-Path -Parent $activeVrClient)
            }
        } catch {
            # Without SteamVR there is nothing to restore; the OpenXR layer may still be there.
            if (-not $removeOpenXr) { throw }
        }
        if ($resolvedSteamVrRoot) {
            $steamVrBin = Join-Path $resolvedSteamVrRoot "bin"
            if ($manifests.Count -eq 0 -and -not (Get-DvrUnrecordedInstall -SteamVrRoot $resolvedSteamVrRoot)) {
                Write-Host "The layer is not installed in $steamVrBin; SteamVR is already stock."
                $removeOpenVr = $false
            }
        } else {
            $removeOpenVr = $false
        }
    }
    if ($removeOpenXr -and -not (Test-DvrOpenXrLayerRegistered)) {
        Write-Host "The OpenXR layer is not registered."
        $removeOpenXr = $false
    }
    if (-not $removeOpenVr -and -not $removeOpenXr) {
        return
    }

    if ($removeOpenVr) {
        $running = @(Get-DvrRunningSteamVrProcesses)
        if ($running.Count -gt 0) {
            $failures.Add("OpenVR: SteamVR is running ($(@($running.ProcessName | Sort-Object -Unique) -join ', ')). Close SteamVR and run the uninstaller again.")
            $removeOpenVr = $false
        }
    }

    if ($removeOpenVr -or $removeOpenXr) {
        if (-not $Yes) {
            Write-Host ""
            Write-Host "This will:"
            if ($removeOpenVr) { Write-Host "  - OpenVR: remove the layer from $steamVrBin and restore SteamVR" }
            if ($removeOpenXr) { Write-Host "  - OpenXR: unregister the API layer for the current user" }
            $answer = Read-Host "Continue? [y/N]"
            if ($answer -notmatch '^(y|yes)$') {
                Write-Host "Nothing was changed."
                return
            }
        }
    }

    # OpenXR is per user (HKCU), so it runs here and not in the elevated copy below, which
    # may belong to a different Windows account.
    if ($removeOpenXr) {
        try {
            & (Join-Path $PSScriptRoot "openxr\uninstall.ps1") | ForEach-Object { Write-Host $_ }
            $removed.Add("OpenXR")
        } catch {
            $failures.Add("OpenXR: $($_.Exception.Message)")
        }
    }

    if ($removeOpenVr) {
        try {
            $childParameters = @{}
            foreach ($entry in $PSBoundParameters.GetEnumerator()) { $childParameters[$entry.Key] = $entry.Value }
            $childParameters["SteamVrRoot"] = $resolvedSteamVrRoot
            $childParameters["SkipOpenXr"] = [switch]$true
            $childParameters["Yes"] = [switch]$true
            if (-not (Invoke-DvrElevatedIfNeeded -ScriptPath $PSCommandPath -Directory $steamVrBin -BoundParameters $childParameters)) {
                Uninstall-DvrOpenVr -SteamVrRoot $resolvedSteamVrRoot
            }
            $removed.Add("OpenVR")
        } catch {
            $failures.Add("OpenVR: $($_.Exception.Message)")
        }
    }

    if ($removed.Count -gt 0) {
        Write-Host ""
        Write-Host "Uninstalled: $($removed -join ' and ')."
        if ($removed.Contains("OpenVR")) { Write-Host "SteamVR runs without the layer." }
        if ($removed.Contains("OpenXR")) {
            Write-Host "OpenXR files and settings remain in $(Get-DvrOpenXrDirectory); delete that folder to remove them."
        }
    }
    if ($failures.Count -gt 0) {
        Write-Host ""
        foreach ($failure in $failures) {
            Write-Host "Uninstall failed: $failure" -ForegroundColor Red
        }
        $exitCode = 1
    }
} catch {
    Write-Host ""
    Write-Host "Uninstall failed: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
} finally {
    if ($PauseAtEnd) {
        Read-Host "Press Enter to close" | Out-Null
    }
}
exit $exitCode
