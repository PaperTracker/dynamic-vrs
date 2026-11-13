Set-StrictMode -Version Latest

# Helpers for the OpenVR integration: finding SteamVR, the install manifests and the leftovers of
# installations that have none. Runs on Windows PowerShell 5.1, which is what a stock Windows install has.

. (Join-Path $PSScriptRoot "common.ps1")

$script:DvrSteamVrProcessNames = @("vrserver", "vrcompositor", "vrmonitor", "vrwebhelper", "vrstartup")

function Get-DvrDefaultBackupRoot {
    return (Join-Path (Get-DvrDataRoot) "backups")
}

# Finds the SteamVR install directory: an explicit path, then the runtime SteamVR itself
# registers in openvrpaths.vrpath, then every Steam library folder.
function Resolve-DvrSteamVrRoot {
    param([string]$RequestedRoot)

    if ($RequestedRoot) {
        return [System.IO.Path]::GetFullPath($RequestedRoot)
    }

    $candidates = [System.Collections.Generic.List[string]]::new()
    $vrPathFile = Join-Path $env:LOCALAPPDATA "openvr\openvrpaths.vrpath"
    if (Test-Path -LiteralPath $vrPathFile -PathType Leaf) {
        try {
            $vrPaths = Get-Content -LiteralPath $vrPathFile -Raw | ConvertFrom-Json
            if ($vrPaths.PSObject.Properties["runtime"]) {
                foreach ($runtime in @($vrPaths.runtime)) {
                    if ($runtime) { $candidates.Add([string]$runtime) }
                }
            }
        } catch {
            Write-Verbose "Cannot read ${vrPathFile}: $_"
        }
    }

    $steamRoots = [System.Collections.Generic.List[string]]::new()
    foreach ($registryPath in @(
        "HKCU:\Software\Valve\Steam",
        "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam"
    )) {
        if (-not (Test-Path $registryPath)) {
            continue
        }
        $properties = Get-ItemProperty $registryPath
        foreach ($propertyName in @("SteamPath", "InstallPath")) {
            $property = $properties.PSObject.Properties[$propertyName]
            $value = if ($property) { $property.Value } else { $null }
            if ($value) {
                $steamRoots.Add([string]$value)
            }
        }
    }
    foreach ($steamRoot in $steamRoots) {
        $candidates.Add((Join-Path $steamRoot "steamapps\common\SteamVR"))
        $libraryFile = Join-Path $steamRoot "steamapps\libraryfolders.vdf"
        if (Test-Path -LiteralPath $libraryFile -PathType Leaf) {
            $libraryText = Get-Content -LiteralPath $libraryFile -Raw
            foreach ($match in [regex]::Matches($libraryText, '"path"\s+"([^"]+)"')) {
                $library = $match.Groups[1].Value.Replace('\\', '\')
                $candidates.Add((Join-Path $library "steamapps\common\SteamVR"))
            }
        }
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate "bin\vrclient_x64.dll") -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    throw "SteamVR was not found. Start SteamVR once so it registers itself, or pass -SteamVrRoot explicitly."
}

function Get-DvrRunningSteamVrProcesses {
    return @(Get-Process -Name $script:DvrSteamVrProcessNames -ErrorAction SilentlyContinue)
}

# Throws when SteamVR\bin\vrclient_x64.dll cannot be replaced because a process has it loaded.
# Steam itself loads it once SteamVR has run, so closing SteamVR alone is not enough.
function Assert-DvrVrClientReplaceable {
    param([Parameter(Mandatory = $true)][string]$SteamVrBin)

    $vrClient = Join-Path $SteamVrBin "vrclient_x64.dll"
    if (-not (Test-Path -LiteralPath $vrClient -PathType Leaf)) {
        return
    }
    try {
        $stream = [System.IO.File]::Open($vrClient, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        $stream.Dispose()
        return
    } catch [System.IO.IOException] {
        Write-Verbose "vrclient_x64.dll is in use: $_"
    }

    $holders = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
        try {
            @($_.Modules | Where-Object { $_.FileName -eq $vrClient }).Count -gt 0
        } catch {
            $false
        }
    } | ForEach-Object { "$($_.ProcessName).exe" } | Sort-Object -Unique)
    $who = if ($holders.Count -gt 0) { " by " + ($holders -join ", ") } else { "" }
    throw ("$vrClient is in use$who. Exit Steam completely (Steam menu > Exit, not just closing " +
        "the window) and any VR application, then run this again.")
}

# Install manifests whose installation has not been restored yet, newest first.
function Get-DvrActiveInstallManifests {
    param([string]$BackupRoot = (Get-DvrDefaultBackupRoot))

    if (-not (Test-Path -LiteralPath $BackupRoot -PathType Container)) {
        return @()
    }
    return @(
        Get-ChildItem -LiteralPath $BackupRoot -Directory |
            Where-Object {
                (Test-Path -LiteralPath (Join-Path $_.FullName "install_manifest.json") -PathType Leaf) -and
                -not (Test-Path -LiteralPath (Join-Path $_.FullName "restored.txt"))
            } |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName "install_manifest.json" }
    )
}

# True for the layer's vrclient_x64.dll proxy, which logs this stage; SteamVR's own client
# library never contains the string.
function Test-DvrLayerProxy {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    return [System.Text.Encoding]::ASCII.GetString($bytes).Contains("stage=payload-config")
}

# Describes layer files in SteamVR\bin that no install manifest accounts for, for example
# from a manual install or a manifest that was deleted. Returns $null when there are none.
function Get-DvrUnrecordedInstall {
    param([Parameter(Mandatory = $true)][string]$SteamVrRoot)

    $steamVrBin = Join-Path $SteamVrRoot "bin"
    $activeVrClient = Join-Path $steamVrBin "vrclient_x64.dll"
    $leftovers = @(
        @("real_vrclient_x64.dll", "dynamic_vrs_real", "dynamic-vrs_payload") |
            ForEach-Object { Join-Path $steamVrBin $_ } |
            Where-Object { Test-Path -LiteralPath $_ }
    )
    $proxyActive = Test-DvrLayerProxy -Path $activeVrClient
    if (-not $proxyActive -and $leftovers.Count -eq 0) {
        return $null
    }

    # Where the proxy was told to find SteamVR's library, then the two layouts the scripts create.
    $candidates = [System.Collections.Generic.List[string]]::new()
    $config = Join-Path $steamVrBin "dynamic-vrs.ini"
    if (Test-Path -LiteralPath $config -PathType Leaf) {
        $match = [regex]::Match((Get-Content -LiteralPath $config -Raw), '(?m)^realVrClientPath=([^\r\n]*)')
        $configured = if ($match.Success) { $match.Groups[1].Value.Trim() } else { "" }
        if ($configured -and -not [System.IO.Path]::IsPathRooted($configured) -and -not $configured.Contains("..")) {
            $candidates.Add($configured)
        }
    }
    $candidates.Add("real_vrclient_x64.dll")
    $candidates.Add("dynamic_vrs_real\vrclient_x64.dll")

    $realVrClient = $null
    foreach ($candidate in $candidates) {
        $path = Join-Path $steamVrBin $candidate
        if ((Test-Path -LiteralPath $path -PathType Leaf) -and -not (Test-DvrLayerProxy -Path $path)) {
            $exports = Get-DvrPeExportNames -Path $path
            if ($exports -contains "VRClientCoreFactory" -and $exports -contains "HmdSystemFactory") {
                $realVrClient = $path
                break
            }
        }
    }

    return [PSCustomObject]@{
        SteamVrBin = $steamVrBin
        ActiveVrClient = $activeVrClient
        ProxyActive = $proxyActive
        RealVrClient = $realVrClient
        Leftovers = $leftovers
        Config = $config
    }
}

# Puts SteamVR's own vrclient_x64.dll back and removes the layer files an unrecorded install
# left behind. Everything is copied to a new backup directory first. dynamic-vrs.ini stays.
function Remove-DvrUnrecordedInstall {
    param(
        [Parameter(Mandatory = $true)][object]$Install,
        [string]$BackupRoot = (Get-DvrDefaultBackupRoot)
    )

    if ($Install.ProxyActive -and -not $Install.RealVrClient) {
        throw ("SteamVR\bin\vrclient_x64.dll is the layer proxy, but SteamVR's own copy was not found next to it. " +
            "In Steam, open SteamVR > Properties > Installed Files > Verify integrity, then run this again.")
    }

    $backupDirectory = Join-Path $BackupRoot ("unrecorded-" + (Get-Date -Format "yyyyMMdd-HHmmss-fff"))
    New-Item -ItemType Directory -Path $backupDirectory | Out-Null
    Copy-Item -LiteralPath $Install.ActiveVrClient -Destination $backupDirectory
    foreach ($path in $Install.Leftovers) {
        Copy-Item -LiteralPath $path -Destination $backupDirectory -Recurse
    }
    if (Test-Path -LiteralPath $Install.Config -PathType Leaf) {
        Copy-Item -LiteralPath $Install.Config -Destination $backupDirectory
    }

    if ($Install.ProxyActive) {
        Copy-Item -LiteralPath $Install.RealVrClient -Destination $Install.ActiveVrClient -Force
        if ((Get-FileHash -LiteralPath $Install.ActiveVrClient).Hash -ne (Get-FileHash -LiteralPath $Install.RealVrClient).Hash) {
            throw "Restoring SteamVR's vrclient_x64.dll failed; a copy of every file is in $backupDirectory."
        }
    }
    foreach ($path in $Install.Leftovers) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $backupDirectory "README.txt"),
        ("Layer files found in $($Install.SteamVrBin) without an install manifest, removed at UTC " +
            [DateTime]::UtcNow.ToString("o") + ". vrclient_x64.dll here is the file that was active then."),
        [System.Text.UTF8Encoding]::new($false))
    $restored = if ($Install.ProxyActive) { "restored SteamVR's vrclient_x64.dll and " } else { "" }
    Write-Host "Found layer files without an install record; $($restored)removed them. Copies: $backupDirectory"
}

# Rewrites the keys that describe where the installed files are, so a kept dynamic-vrs.ini
# matches the layout this installation uses. Every other key is left as the user set it.
function Sync-DvrConfigLayout {
    param(
        [Parameter(Mandatory = $true)][string]$LiveConfig,
        [Parameter(Mandatory = $true)][string]$StagedConfig
    )

    $staged = Get-Content -LiteralPath $StagedConfig -Raw
    $live = Get-Content -LiteralPath $LiveConfig -Raw
    $original = $live
    foreach ($key in @("realVrClientPath", "spoofRealVrClientPath", "payloadDirectory", "nativeLayerLibrary")) {
        $stagedMatch = [regex]::Match($staged, "(?m)^$key=([^\r\n]*)")
        if (-not $stagedMatch.Success) {
            continue
        }
        $line = "$key=$($stagedMatch.Groups[1].Value)"
        if ([regex]::IsMatch($live, "(?m)^$key=")) {
            $live = [regex]::Replace($live, "(?m)^$key=[^\r\n]*", $line.Replace('$', '$$'))
        } else {
            $live = ([regex]"(?m)^\[loader\][^\r\n]*").Replace($live, { param($m) $m.Value + "`r`n" + $line }, 1)
        }
    }
    if ($live -ne $original) {
        [System.IO.File]::WriteAllText($LiveConfig, $live, [System.Text.UTF8Encoding]::new($false))
    }
}

# Lists the names a PE image exports, read from its export directory. Replaces dumpbin,
# so staging works without Visual Studio.
function Get-DvrPeExportNames {
    param([Parameter(Mandatory = $true)][string]$Path)

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -le 0 -or $peOffset + 24 -gt $bytes.Length -or
        [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
        throw "Not a PE image: $Path"
    }
    $coffHeader = $peOffset + 4
    $sectionCount = [BitConverter]::ToUInt16($bytes, $coffHeader + 2)
    $optionalHeaderSize = [BitConverter]::ToUInt16($bytes, $coffHeader + 16)
    $optionalHeader = $coffHeader + 20
    $magic = [BitConverter]::ToUInt16($bytes, $optionalHeader)
    $dataDirectories = $optionalHeader + $(if ($magic -eq 0x20B) { 112 } else { 96 })
    $exportRva = [BitConverter]::ToUInt32($bytes, $dataDirectories)
    if ($exportRva -eq 0) {
        return @()
    }

    $sectionTable = $optionalHeader + $optionalHeaderSize
    $toOffset = {
        param([uint32]$Rva)
        for ($index = 0; $index -lt $sectionCount; ++$index) {
            $section = $sectionTable + 40 * $index
            $virtualSize = [BitConverter]::ToUInt32($bytes, $section + 8)
            $virtualAddress = [BitConverter]::ToUInt32($bytes, $section + 12)
            $rawSize = [BitConverter]::ToUInt32($bytes, $section + 16)
            $rawPointer = [BitConverter]::ToUInt32($bytes, $section + 20)
            $extent = [Math]::Max($virtualSize, $rawSize)
            if ($Rva -ge $virtualAddress -and $Rva -lt $virtualAddress + $extent) {
                return [int]($Rva - $virtualAddress + $rawPointer)
            }
        }
        throw "RVA 0x$($Rva.ToString('X')) is outside every section of $Path"
    }

    $exportDirectory = & $toOffset $exportRva
    $nameCount = [BitConverter]::ToUInt32($bytes, $exportDirectory + 24)
    $nameTable = & $toOffset ([BitConverter]::ToUInt32($bytes, $exportDirectory + 32))
    $names = [System.Collections.Generic.List[string]]::new()
    for ($index = 0; $index -lt $nameCount; ++$index) {
        $nameOffset = & $toOffset ([BitConverter]::ToUInt32($bytes, $nameTable + 4 * $index))
        $end = [Array]::IndexOf($bytes, [byte]0, $nameOffset)
        $names.Add([System.Text.Encoding]::ASCII.GetString($bytes, $nameOffset, $end - $nameOffset))
    }
    return $names.ToArray()
}
