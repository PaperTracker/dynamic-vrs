[CmdletBinding()]
param(
    [string]$SteamVrRoot,
    # Rendering mode written to a new dynamic-vrs.ini. An existing one is kept as it is.
    [ValidateSet("gaze", "fixed")]
    [string]$Mode = "gaze",
    # Directory holding the built binaries. Defaults to bin\ in a local package, or the
    # build.ps1 output in a source checkout.
    [string]$BinDirectory,
    # Install only one integration. By default both OpenVR and OpenXR are installed.
    [switch]$SkipOpenVr,
    [switch]$SkipOpenXr,
    # Install without asking for confirmation.
    [switch]$Yes,
    # Wait for Enter before exiting, so a double-clicked window stays readable.
    [switch]$PauseAtEnd
)

# One-step install for end users. OpenVR: removes an earlier installation (keeping its
# settings), stages the runtime against the current SteamVR and installs it with
# the scripts in openvr\. OpenXR: registers the
# API layer for the current user with openxr\install.ps1. The two parts are independent; one
# that cannot run, such as OpenVR while SteamVR is running, does not stop the other.

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "lib\steamvr.ps1")
. (Join-Path $PSScriptRoot "lib\openxr.ps1")

# Replaces an earlier OpenVR installation with this one. Runs elevated when SteamVR\bin needs it.
function Install-DvrOpenVr {
    param(
        [Parameter(Mandatory = $true)][string]$SteamVrRoot,
        [Parameter(Mandatory = $true)][string]$BinDirectory,
        [Parameter(Mandatory = $true)][string]$Mode
    )

    $steamVrBin = Join-Path $SteamVrRoot "bin"
    Assert-DvrVrClientReplaceable -SteamVrBin $steamVrBin

    $previous = @(Get-DvrActiveInstallManifests)
    foreach ($manifest in $previous) {
        Write-Host "Removing the earlier installation recorded in $manifest"
        & (Join-Path $PSScriptRoot "openvr\restore.ps1") -InstallManifestPath $manifest `
            -RemoveHelperFiles -KeepConfig -Confirm:$false
    }
    # Layer files no manifest accounts for, such as a manual install; checked after the
    # recorded installations are gone so their files are not mistaken for such leftovers.
    $unrecorded = Get-DvrUnrecordedInstall -SteamVrRoot $SteamVrRoot
    if ($unrecorded) {
        Remove-DvrUnrecordedInstall -Install $unrecorded
    }

    $stageDirectory = Join-Path (Get-DvrDataRoot) ("staging\" + (Get-Date -Format "yyyyMMdd-HHmmss-fff"))
    $stageArguments = @{
        SteamVrRoot = $SteamVrRoot
        BuildDirectory = $BinDirectory
        OutputDirectory = $stageDirectory
        Mode = $Mode
    }
    & (Join-Path $PSScriptRoot "openvr\stage.ps1") @stageArguments | Out-Null

    & (Join-Path $PSScriptRoot "openvr\install.ps1") -StageDirectory $stageDirectory `
        -SteamVrRoot $SteamVrRoot -KeepExistingConfig -Confirm:$false

    # Initializes SteamVR's client through the installed proxy as a background application
    # (which never starts SteamVR) and checks that both unload again. A proxy SteamVR does
    # not accept is taken out here, before Steam or a game runs into it.
    $probeOutput = & (Join-Path $BinDirectory "dynamic_vrs_proxy_probe.exe") `
        (Join-Path $steamVrBin "vrclient_x64.dll") --init-background 2>&1
    if ($LASTEXITCODE -ne 0) {
        $probeExit = $LASTEXITCODE
        $probeOutput | ForEach-Object { Write-Host "  $_" }
        foreach ($manifest in @(Get-DvrActiveInstallManifests)) {
            & (Join-Path $PSScriptRoot "openvr\restore.ps1") -InstallManifestPath $manifest `
                -RemoveHelperFiles -KeepConfig -Confirm:$false
        }
        throw "SteamVR's client did not work through the installed layer (probe exit code $probeExit). SteamVR was restored."
    }
}

$exitCode = 0
$installed = [System.Collections.Generic.List[string]]::new()
$failures = [System.Collections.Generic.List[string]]::new()
try {
    if ($SkipOpenVr -and $SkipOpenXr) {
        throw "Nothing to install: both -SkipOpenVr and -SkipOpenXr were given."
    }
    $installOpenVr = -not $SkipOpenVr
    $installOpenXr = -not $SkipOpenXr

    $packageRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
    if (-not $BinDirectory) {
        $packagedBin = Join-Path $packageRoot "bin"
        $BinDirectory = if (Test-Path -LiteralPath $packagedBin -PathType Container) {
            $packagedBin
        } else {
            Join-Path $packageRoot "out\dynamic-vrs\build\Release"
        }
    }
    $BinDirectory = [System.IO.Path]::GetFullPath($BinDirectory)
    $required = @(if ($installOpenVr) { "vrclient_x64.dll" }; if ($installOpenXr) { "DynamicVRSLayer64.dll" })
    foreach ($name in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $BinDirectory $name) -PathType Leaf)) {
            throw "The layer binaries were not found in $BinDirectory."
        }
    }

    Write-Host "Dynamic VRS installer"
    Write-Host "  Binaries: $BinDirectory"

    $resolvedSteamVrRoot = $null
    $steamVrBin = $null
    if ($installOpenVr) {
        try {
            $resolvedSteamVrRoot = Resolve-DvrSteamVrRoot -RequestedRoot $SteamVrRoot
        } catch {
            # Someone who only plays OpenXR games on another runtime has no SteamVR to find.
            if (-not $installOpenXr) { throw }
            Write-Warning "$($_.Exception.Message) Skipping the OpenVR integration."
            $installOpenVr = $false
        }
    }
    if ($installOpenVr) {
        $steamVrBin = Join-Path $resolvedSteamVrRoot "bin"
        Write-Host "  SteamVR: $resolvedSteamVrRoot"
    }

    $gpus = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction SilentlyContinue |
        ForEach-Object { [string]$_.Name })
    if (-not ($gpus -match "NVIDIA")) {
        Write-Warning "No NVIDIA GPU was found ($($gpus -join ', ')). The layer only shades with NVIDIA VRS; elsewhere it loads and does nothing."
    }

    if ($installOpenVr) {
        $running = @(Get-DvrRunningSteamVrProcesses)
        if ($running.Count -gt 0) {
            $failures.Add("OpenVR: SteamVR is running ($(@($running.ProcessName | Sort-Object -Unique) -join ', ')). Close SteamVR and run the installer again.")
            $installOpenVr = $false
        }
    }

    if ($installOpenVr -or $installOpenXr) {
        if (-not $Yes) {
            Write-Host ""
            Write-Host "This will:"
            if ($installOpenVr) {
                $vrClient = Join-Path $steamVrBin "vrclient_x64.dll"
                Write-Host "  - OpenVR: replace $vrClient with the Dynamic VRS proxy and keep a backup of the original"
            }
            if ($installOpenXr) {
                Write-Host "  - OpenXR: register the API layer for the current user (files in $(Get-DvrOpenXrDirectory))"
            }
            $answer = Read-Host "Continue? [y/N]"
            if ($answer -notmatch '^(y|yes)$') {
                Write-Host "Nothing was changed."
                return
            }
        }

        # Downloaded files carry a Zone.Identifier stream; drop it so Windows does not treat
        # the installed copies as files from the internet.
        Get-ChildItem -LiteralPath $BinDirectory, $PSScriptRoot -File -Recurse -ErrorAction SilentlyContinue |
            Unblock-File -ErrorAction SilentlyContinue
    }

    # OpenXR is per user (HKCU and LOCALAPPDATA), so it runs here and not in the elevated
    # copy below, which may belong to a different Windows account.
    if ($installOpenXr) {
        try {
            & (Join-Path $PSScriptRoot "openxr\install.ps1") -BinDirectory $BinDirectory -Mode $Mode |
                ForEach-Object { Write-Host $_ }
            $installed.Add("OpenXR")
        } catch {
            $failures.Add("OpenXR: $($_.Exception.Message)")
        }
    }

    if ($installOpenVr) {
        try {
            $childParameters = @{}
            foreach ($entry in $PSBoundParameters.GetEnumerator()) { $childParameters[$entry.Key] = $entry.Value }
            $childParameters["SteamVrRoot"] = $resolvedSteamVrRoot
            $childParameters["SkipOpenXr"] = [switch]$true
            $childParameters["Yes"] = [switch]$true
            if (-not (Invoke-DvrElevatedIfNeeded -ScriptPath $PSCommandPath -Directory $steamVrBin -BoundParameters $childParameters)) {
                Install-DvrOpenVr -SteamVrRoot $resolvedSteamVrRoot -BinDirectory $BinDirectory -Mode $Mode
            }
            $installed.Add("OpenVR")
        } catch {
            $failures.Add("OpenVR: $($_.Exception.Message)")
        }
    }

    if ($installed.Count -gt 0) {
        Write-Host ""
        Write-Host "Installed: $($installed -join ' and ')."
        Write-Host "Start a compatible Direct3D 11 VR application. The control panel opens by default after frame submission."
        Write-Host "Do not start games protected by an anti-cheat (Easy Anti-Cheat, for example) while it is installed."
        if ($installed.Contains("OpenVR")) {
            Write-Host "  OpenVR settings: $(Join-Path $steamVrBin 'dynamic-vrs.ini')"
            Write-Host "  If a SteamVR update restores the original DLL, repeat the OpenVR install to use it again."
        }
        if ($installed.Contains("OpenXR")) {
            Write-Host "  OpenXR settings: $(Join-Path (Get-DvrOpenXrDirectory) 'dynamic-vrs.ini')"
            Write-Host "  If a Steam-launched OpenXR game does not load the layer, exit Steam completely and start it again."
        }
        Write-Host "  Logs:            $(Join-Path (Get-DvrDataRoot) 'logs')"
        $uninstaller = (Join-Path $PSScriptRoot "uninstall.ps1").Replace("'", "''")
        Write-Host "To uninstall, close VR applications and run in PowerShell: & '$uninstaller'"
    }
    if ($failures.Count -gt 0) {
        Write-Host ""
        foreach ($failure in $failures) {
            Write-Host "Installation failed: $failure" -ForegroundColor Red
        }
        $exitCode = 1
    }
} catch {
    Write-Host ""
    Write-Host "Installation failed: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
} finally {
    if ($PauseAtEnd) {
        Read-Host "Press Enter to close" | Out-Null
    }
}
exit $exitCode
