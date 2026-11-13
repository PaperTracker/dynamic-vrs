param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$')]
    [string]$Version,
    # Directory holding the built binaries; defaults to the build.ps1 output.
    [string]$BuildDirectory,
    [string]$OutputDirectory
)

# Creates a local ZIP with the binaries, installer launchers and documentation, plus a
# SHA-256 file. Run after build.ps1 or from the CI packaging check.

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $repositoryRoot "out\dynamic-vrs\build\Release"
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repositoryRoot "out\release"
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

$binaries = @(
    "vrclient_x64.dll",
    "DynamicVRSLayer64.dll",
    "DynamicVRSPanel64.exe",
    "dynamic_vrs_proxy_probe.exe"
)
foreach ($name in $binaries) {
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory $name) -PathType Leaf)) {
        throw "Build output not found: $(Join-Path $BuildDirectory $name)"
    }
}

# A layer built without NVAPI loads but never shades; refuse to ship one. This log message
# only exists in that build.
$layerBytes = [System.IO.File]::ReadAllBytes((Join-Path $BuildDirectory "DynamicVRSLayer64.dll"))
$layerText = [System.Text.Encoding]::ASCII.GetString($layerBytes)
if ($layerText.Contains("layer-built-without-nvapi")) {
    throw "DynamicVRSLayer64.dll was built without NVAPI_ROOT and cannot apply VRS."
}
if (-not $layerText.Contains("stage=eye-projection result=ready")) {
    throw "DynamicVRSLayer64.dll was built without OPENVR_SDK_ROOT. The package includes the OpenVR installer."
}
if (-not $layerText.Contains("stage=openxr-instance")) {
    throw "DynamicVRSLayer64.dll was built without OPENXR_SDK_ROOT. The package includes the OpenXR installer."
}

$packageName = "DynamicVRS-$Version"
$packageRoot = Join-Path $OutputDirectory $packageName
$zipPath = Join-Path $OutputDirectory "$packageName-win64.zip"
foreach ($path in @($packageRoot, $zipPath, "$zipPath.sha256")) {
    $expectedPrefix = $OutputDirectory.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not ([IO.Path]::GetFullPath($path)).StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package output escaped the output directory: $path"
    }
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}

function Copy-PackageFile {
    param([string]$Source, [string]$Destination)
    $target = Join-Path $packageRoot $Destination
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $target
}

foreach ($name in $binaries) {
    Copy-PackageFile (Join-Path $BuildDirectory $name) "bin\$name"
}
$scriptRoot = Join-Path $repositoryRoot "scripts"
foreach ($script in Get-ChildItem -LiteralPath $scriptRoot -Filter *.ps1 -File -Recurse) {
    Copy-PackageFile $script.FullName ("scripts\" + $script.FullName.Substring($scriptRoot.Length + 1))
}
Copy-PackageFile (Join-Path $repositoryRoot "config\dynamic-vrs.ini.example") "config\dynamic-vrs.ini.example"
Copy-PackageFile (Join-Path $repositoryRoot "config\openxr-layer.json") "config\openxr-layer.json"
Copy-PackageFile (Join-Path $PSScriptRoot "Install.cmd") "Install.cmd"
Copy-PackageFile (Join-Path $PSScriptRoot "Uninstall.cmd") "Uninstall.cmd"
Copy-PackageFile (Join-Path $repositoryRoot "Install-OpenXrOnly.cmd") "Install-OpenXrOnly.cmd"
Copy-PackageFile (Join-Path $repositoryRoot "NOTICE-ACCEPTABLE-USE.md") "NOTICE-ACCEPTABLE-USE.md"
Copy-PackageFile (Join-Path $repositoryRoot "LICENSE") "LICENSE.txt"
Copy-PackageFile (Join-Path $repositoryRoot "THIRD-PARTY-NOTICES.md") "THIRD-PARTY-NOTICES.md"

$readme = (Get-Content -LiteralPath (Join-Path $PSScriptRoot "README.txt") -Raw).Replace("@VERSION@", $Version)
$readme = $readme.Replace("`r`n", "`n").Replace("`n", "`r`n")
[System.IO.File]::WriteAllText((Join-Path $packageRoot "README.txt"), $readme, [System.Text.UTF8Encoding]::new($false))

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $packageRoot, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $true)
$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText("$zipPath.sha256", "$hash  $(Split-Path -Leaf $zipPath)`n",
    [System.Text.UTF8Encoding]::new($false))

Write-Output "Package: $zipPath"
Write-Output "SHA-256: $hash"
