param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$OpenVrSdkRoot = (Join-Path $env:USERPROFILE ".cache\openvr-sdk\openvr"),
    [string]$NvApiRoot = (Join-Path $env:USERPROFILE ".cache\openvr-sdk\nvapi"),
    [string]$OpenXrSdkRoot = (Join-Path $env:USERPROFILE ".cache\openxr-sdk"),
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$toolRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$repositoryRoot = $toolRoot
$buildRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot "out\dynamic-vrs\build"))
$expectedBuildPrefix = [System.IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot "out\dynamic-vrs")) + [System.IO.Path]::DirectorySeparatorChar
$cmakeLauncher = Join-Path $repositoryRoot "tools\ci\cmake_pinned.cmd"
$toolsetFile = Join-Path $repositoryRoot "cmake\pinned_msvc_toolset.txt"

if (-not (Test-Path -LiteralPath $cmakeLauncher -PathType Leaf)) {
    throw "Pinned CMake launcher not found: $cmakeLauncher"
}
if (-not (Test-Path -LiteralPath $toolsetFile -PathType Leaf)) {
    throw "Pinned MSVC version file not found: $toolsetFile"
}
if (-not $buildRoot.StartsWith($expectedBuildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use a build directory outside the expected output root: $buildRoot"
}

if ($Clean -and (Test-Path -LiteralPath $buildRoot)) {
    Remove-Item -LiteralPath $buildRoot -Recurse -Force
}

$pinnedToolset = (Get-Content -LiteralPath $toolsetFile -Raw).Trim()

$cmakeArguments = @(
    "-S", $toolRoot,
    "-B", $buildRoot,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-T", "version=$pinnedToolset"
)
$sdkOptions = [ordered]@{
    OPENVR_SDK_ROOT = $OpenVrSdkRoot
    NVAPI_ROOT = $NvApiRoot
    OPENXR_SDK_ROOT = $OpenXrSdkRoot
}
foreach ($option in $sdkOptions.GetEnumerator()) {
    $sdkPath = if ($option.Value -and (Test-Path -LiteralPath $option.Value -PathType Container)) {
        [System.IO.Path]::GetFullPath($option.Value)
    } else { "" }
    # Always pass the option, including an empty value, to clear a previous SDK cache entry.
    $cmakeArguments += "-D$($option.Key)=$sdkPath"
}

& $cmakeLauncher @cmakeArguments
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE"
}

& $cmakeLauncher --build $buildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

$outputDirectory = Join-Path $buildRoot $Configuration
$proxyPath = Join-Path $outputDirectory "vrclient_x64.dll"
$probePath = Join-Path $outputDirectory "dynamic_vrs_proxy_probe.exe"
foreach ($file in @($proxyPath, $probePath)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Expected build output was not produced: $file"
    }
}

Write-Output "Build complete:"
Write-Output "  Proxy: $proxyPath"
Write-Output "  Probe: $probePath"
