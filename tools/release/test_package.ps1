param(
    # SteamVR's own bin\vrclient_x64.dll, used to build the fake SteamVR trees. Defaults to the
    # installed SteamVR's file when that is not the layer proxy.
    [string]$StockVrClient,
    # Scratch directory; deleted and recreated.
    [string]$Work = (Join-Path $env:TEMP "dynamic-vrs-package-test")
)

# Release-package test. Builds the zip with package.ps1 from the build.ps1 output, extracts it
# like a download, and drives Install.cmd and Uninstall.cmd (answering their prompts) against
# fake SteamVR trees:
#   A. a hand install without an install record, in the layout older scripts produced;
#   B. stock SteamVR through install, reinstall, a SteamVR update and uninstall.
# After every install dynamic_vrs_proxy_probe.exe --init-background loads the installed proxy,
# which has to find SteamVR's client through the installed layout, initialize it, and take
# it along when it is unloaded (the installer runs the same check). LOCALAPPDATA is
# redirected; the real SteamVR and backups are never written. The launchers run with -SkipOpenXr
# because the OpenXR registration is in HKCU, which is not redirected. Not run in CI: it needs
# SteamVR's client library, which cannot be redistributed.

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "..\..\scripts\lib\steamvr.ps1")

$script:failures = 0
$script:lastExit = 0
$script:lastOutput = ""

function Check([string]$Name, [bool]$Ok) {
    if ($Ok) {
        Write-Host "  PASS $Name" -ForegroundColor Green
    } else {
        Write-Host "  FAIL $Name" -ForegroundColor Red
        $script:failures++
    }
}

function Hash([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) { (Get-FileHash -LiteralPath $Path).Hash } else { "<missing>" }
}

function IniValue([string]$Path, [string]$Key) {
    $match = [regex]::Match((Get-Content -LiteralPath $Path -Raw), "(?m)^$Key=([^\r\n]*)")
    if ($match.Success) { $match.Groups[1].Value } else { "<absent>" }
}

if (-not $StockVrClient) {
    $StockVrClient = Join-Path (Resolve-DvrSteamVrRoot) "bin\vrclient_x64.dll"
}
$StockVrClient = [System.IO.Path]::GetFullPath($StockVrClient)
if (Test-DvrLayerProxy -Path $StockVrClient) {
    throw "$StockVrClient is the layer proxy. Pass SteamVR's own client with -StockVrClient."
}
$stockHash = Hash $StockVrClient

if (Test-Path -LiteralPath $Work) {
    Remove-Item -LiteralPath $Work -Recurse -Force
}
New-Item -ItemType Directory -Path $Work | Out-Null
$realVrPathFile = Join-Path $env:LOCALAPPDATA "openvr\openvrpaths.vrpath"
$env:LOCALAPPDATA = Join-Path $Work "LocalAppData"
New-Item -ItemType Directory -Path (Join-Path $env:LOCALAPPDATA "openvr") | Out-Null
# SteamVR's client finds its runtime folder (resources, drivers) through this file. The probe
# initializes it from the fake trees, which only hold the client library.
Copy-Item -LiteralPath $realVrPathFile -Destination (Join-Path $env:LOCALAPPDATA "openvr")
$backupRoot = Join-Path $env:LOCALAPPDATA "OpenVRDynamicVRS\backups"

Write-Host "--- package"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "package.ps1") `
    -Version 0.0.0-test -OutputDirectory (Join-Path $Work "release") | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "package.ps1 failed; run build.ps1 first."
}
Expand-Archive -LiteralPath (Join-Path $Work "release\DynamicVRS-0.0.0-test-win64.zip") `
    -DestinationPath (Join-Path $Work "Downloads")
$pkg = Join-Path $Work "Downloads\DynamicVRS-0.0.0-test"
$proxyHash = Hash (Join-Path $pkg "bin\vrclient_x64.dll")
Check "zip extracts with Install.cmd at the top" (Test-Path (Join-Path $pkg "Install.cmd"))

function Run-Launcher([string]$Launcher, [string]$Root, [string]$Answer = "y") {
    $output = ($Answer + "`r`n`r`n") | cmd /c "`"$(Join-Path $pkg $Launcher)`" -SteamVrRoot `"$Root`" -SkipOpenXr" 2>&1
    $script:lastExit = $LASTEXITCODE
    $script:lastOutput = ($output | Out-String)
    $output | ForEach-Object { Write-Host "    | $_" }
}

function Probe([string]$Bin) {
    $output = & (Join-Path $pkg "bin\dynamic_vrs_proxy_probe.exe") (Join-Path $Bin "vrclient_x64.dll") --init-background 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Host "    probe| $_" }
    }
    return $LASTEXITCODE -eq 0
}

# ------------------------------------------------------------------ A: unrecorded hand install

Write-Host "--- A. hand install without an install record (older layout)"
$rootA = Join-Path $Work "A\SteamVR"
$binA = Join-Path $rootA "bin"
New-Item -ItemType Directory -Path "$binA\dynamic_vrs_real", "$binA\dynamic-vrs_payload" | Out-Null
# An older proxy build: the packaged one with bytes appended, so its hash differs.
[IO.File]::WriteAllBytes("$binA\vrclient_x64.dll",
    [IO.File]::ReadAllBytes((Join-Path $pkg "bin\vrclient_x64.dll")) + [byte[]](9, 9))
Copy-Item -LiteralPath $StockVrClient -Destination "$binA\dynamic_vrs_real\vrclient_x64.dll"
foreach ($name in @("DynamicVRSLayer64.dll", "DynamicVRSPanel64.exe")) {
    Copy-Item -LiteralPath (Join-Path $pkg "bin\$name") -Destination "$binA\dynamic-vrs_payload\$name"
    Copy-Item -LiteralPath (Join-Path $pkg "bin\$name") -Destination "$binA\dynamic-vrs_payload\$name.old"
}
$handIni = (Get-Content -LiteralPath (Join-Path $pkg "config\dynamic-vrs.ini.example") -Raw) -replace "`r`n", "`n"
foreach ($pair in @(
    @("(?m)^enabled=0$", "enabled=1`ntargetProcess=VRChat.exe"),
    @("(?m)^nativeVrsMode=off$", "nativeVrsMode=fixed"),
    @("(?m)^realVrClientPath=.*$", "realVrClientPath=dynamic_vrs_real\vrclient_x64.dll"),
    @("(?m)^spoofRealVrClientPath=0$", "spoofRealVrClientPath=1"),
    @("(?m)^vrsRing1x1Degrees=.*$", "vrsRing1x1Degrees=12")
)) {
    $handIni = [regex]::Replace($handIni, $pair[0], $pair[1].Replace('$', '$$'))
}
[IO.File]::WriteAllText("$binA\dynamic-vrs.ini", $handIni)
[IO.File]::WriteAllText("$binA\dynamic-vrs.ini.bak", "user backup")
$handProxyHash = Hash "$binA\vrclient_x64.dll"
$userKeys = [ordered]@{}
foreach ($key in @("enabled", "nativeVrsMode", "vrsRing1x1Degrees", "targetProcess", "gazeExportPort")) {
    $userKeys[$key] = IniValue "$binA\dynamic-vrs.ini" $key
}
Check "hand install set up" ($userKeys["targetProcess"] -eq "VRChat.exe" -and
    (IniValue "$binA\dynamic-vrs.ini" "realVrClientPath") -eq "dynamic_vrs_real\vrclient_x64.dll")
Check "hand install loads SteamVR's client" (Probe $binA)

Run-Launcher "Install.cmd" $rootA "n"
Check "answering n changes nothing" ((Hash "$binA\vrclient_x64.dll") -eq $handProxyHash -and (Test-Path "$binA\dynamic_vrs_real"))

Run-Launcher "Install.cmd" $rootA
Check "install exit code 0" ($script:lastExit -eq 0)
Check "active vrclient is the packaged proxy" ((Hash "$binA\vrclient_x64.dll") -eq $proxyHash)
Check "real_vrclient_x64.dll is SteamVR's file" ((Hash "$binA\real_vrclient_x64.dll") -eq $stockHash)
Check "dynamic_vrs_real removed" (-not (Test-Path "$binA\dynamic_vrs_real"))
Check "ini realVrClientPath rewritten" ((IniValue "$binA\dynamic-vrs.ini" "realVrClientPath") -eq "real_vrclient_x64.dll")
Check "ini spoofRealVrClientPath rewritten" ((IniValue "$binA\dynamic-vrs.ini" "spoofRealVrClientPath") -eq "0")
foreach ($key in $userKeys.Keys) {
    Check "ini $key kept ($($userKeys[$key]))" ((IniValue "$binA\dynamic-vrs.ini" $key) -eq $userKeys[$key])
}
Check "payload holds only the new files" (
    (@(Get-ChildItem "$binA\dynamic-vrs_payload" | Sort-Object Name).Name -join ",") -eq "DynamicVRSLayer64.dll,DynamicVRSPanel64.exe")
$unrecordedBackup = @(Get-ChildItem $backupRoot -Directory -Filter "unrecorded-*")
Check "old files copied to an unrecorded- backup" ($unrecordedBackup.Count -eq 1 -and
    (Hash "$($unrecordedBackup[0].FullName)\vrclient_x64.dll") -eq $handProxyHash -and
    (Hash "$($unrecordedBackup[0].FullName)\dynamic_vrs_real\vrclient_x64.dll") -eq $stockHash)
Check "installed proxy loads SteamVR's client" (Probe $binA)

Run-Launcher "Uninstall.cmd" $rootA
Check "uninstall exit code 0" ($script:lastExit -eq 0)
Check "active vrclient is SteamVR's file" ((Hash "$binA\vrclient_x64.dll") -eq $stockHash)
Check "no layer files left" (-not (Test-Path "$binA\real_vrclient_x64.dll") -and
    -not (Test-Path "$binA\dynamic-vrs_payload") -and -not (Test-Path "$binA\dynamic-vrs.ini"))
Check "user's dynamic-vrs.ini.bak left alone" (Test-Path "$binA\dynamic-vrs.ini.bak")
Check "edited ini kept in a backup" (@(Get-ChildItem $backupRoot -Recurse -Filter dynamic-vrs.ini).Count -ge 1)

Run-Launcher "Uninstall.cmd" $rootA
Check "second uninstall has nothing to do" ($script:lastExit -eq 0 -and (Hash "$binA\vrclient_x64.dll") -eq $stockHash)

# ------------------------------------------------------------------ B: stock SteamVR

Write-Host "--- B. stock SteamVR: install, reinstall, SteamVR update, uninstall"
Remove-Item -LiteralPath (Join-Path $env:LOCALAPPDATA "OpenVRDynamicVRS") -Recurse -Force
$rootB = Join-Path $Work "B\SteamVR"
$binB = Join-Path $rootB "bin"
New-Item -ItemType Directory -Path $binB | Out-Null
Copy-Item -LiteralPath $StockVrClient -Destination "$binB\vrclient_x64.dll"
$ini = "$binB\dynamic-vrs.ini"

# Steam keeps vrclient_x64.dll loaded once SteamVR has run; the installer must stop before
# changing anything.
$lock = [IO.File]::Open("$binB\vrclient_x64.dll", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    Run-Launcher "Install.cmd" $rootB
} finally {
    $lock.Dispose()
}
Check "install refuses while vrclient_x64.dll is in use" ($script:lastExit -ne 0 -and $script:lastOutput.Contains("Exit Steam completely"))
Check "nothing changed while it was in use" (@(Get-ChildItem $binB).Count -eq 1 -and
    (Hash "$binB\vrclient_x64.dll") -eq $stockHash -and -not (Test-Path $backupRoot))

Run-Launcher "Install.cmd" $rootB
Check "fresh install" ($script:lastExit -eq 0 -and (Hash "$binB\vrclient_x64.dll") -eq $proxyHash -and
    (Hash "$binB\real_vrclient_x64.dll") -eq $stockHash)
Check "new ini is enabled, gaze mode" ((IniValue $ini "nativeVrsMode") -eq "gaze" -and (IniValue $ini "enabled") -eq "1")
Check "new ini has no executable filter" ((IniValue $ini "targetProcess") -eq "<absent>")
Check "installed proxy loads SteamVR's client" (Probe $binB)

[IO.File]::WriteAllText($ini, ((Get-Content -LiteralPath $ini -Raw) -replace "(?m)^vrsRing1x1Degrees=.*$", "vrsRing1x1Degrees=11"))
Run-Launcher "Install.cmd" $rootB
Check "reinstall keeps an edited setting" ($script:lastExit -eq 0 -and (IniValue $ini "vrsRing1x1Degrees") -eq "11" -and
    (Hash "$binB\vrclient_x64.dll") -eq $proxyHash)

# A SteamVR update replaces the proxy with a new client build.
[IO.File]::WriteAllBytes("$binB\vrclient_x64.dll", [IO.File]::ReadAllBytes($StockVrClient) + [byte[]](1, 2, 3, 4))
$updatedHash = Hash "$binB\vrclient_x64.dll"
Run-Launcher "Install.cmd" $rootB
Check "reinstall after a SteamVR update" ($script:lastExit -eq 0 -and (Hash "$binB\vrclient_x64.dll") -eq $proxyHash -and
    (Hash "$binB\real_vrclient_x64.dll") -eq $updatedHash -and (IniValue $ini "vrsRing1x1Degrees") -eq "11")
Check "installed proxy loads the updated client" (Probe $binB)

Run-Launcher "Uninstall.cmd" $rootB
Check "uninstall restores the updated client and nothing else is left" ($script:lastExit -eq 0 -and
    (Hash "$binB\vrclient_x64.dll") -eq $updatedHash -and @(Get-ChildItem $binB).Count -eq 1)

Write-Host ""
if ($script:failures -eq 0) {
    Write-Host "ALL PASSED" -ForegroundColor Green
} else {
    Write-Host "$($script:failures) FAILED" -ForegroundColor Red
}
exit $script:failures
