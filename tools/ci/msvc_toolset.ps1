Set-StrictMode -Version Latest

function Get-DvrPinnedMsvcToolsetVersion {
    param(
        [string]$RepositoryRoot = (Join-Path $PSScriptRoot "..\..")
    )

    $repositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
    $versionFile = Join-Path $repositoryRoot "cmake\pinned_msvc_toolset.txt"
    if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
        throw "Pinned MSVC toolset version file not found: $versionFile"
    }

    $version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    if ($version -notmatch '^\d+\.\d+\.\d+$') {
        throw "Pinned MSVC toolset version is invalid: '$version' ($versionFile)"
    }
    return $version
}

function Resolve-DvrMsvcToolset {
    param(
        [string]$RepositoryRoot = (Join-Path $PSScriptRoot "..\.."),
        [string]$VsWherePath = (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe")
    )

    if (-not (Test-Path -LiteralPath $VsWherePath -PathType Leaf)) {
        throw "Visual Studio locator not found: $VsWherePath"
    }

    $pinnedVersion = Get-DvrPinnedMsvcToolsetVersion -RepositoryRoot $RepositoryRoot
    $instanceJson = & $VsWherePath -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($instanceJson -join "`n"))) {
        throw "Visual Studio with the x64 C++ toolchain was not found"
    }

    $instances = ($instanceJson -join "`n") | ConvertFrom-Json
    $instances = @($instances | Sort-Object {
            [System.Version]$_.installationVersion
        } -Descending)
    $discoveredVersions = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

    foreach ($instance in $instances) {
        $visualStudioRoot = [string]$instance.installationPath
        $toolsetRoot = Join-Path $visualStudioRoot "VC\Tools\MSVC"
        if (Test-Path -LiteralPath $toolsetRoot -PathType Container) {
            Get-ChildItem -LiteralPath $toolsetRoot -Directory | ForEach-Object {
                $null = $discoveredVersions.Add($_.Name)
            }
        }

        $toolsetPath = Join-Path $toolsetRoot $pinnedVersion
        $vcvarsPath = Join-Path $visualStudioRoot "VC\Auxiliary\Build\vcvars64.bat"
        $compilerPath = Join-Path $toolsetPath "bin\Hostx64\x64\cl.exe"
        if ((Test-Path -LiteralPath $vcvarsPath -PathType Leaf) -and
            (Test-Path -LiteralPath $compilerPath -PathType Leaf)) {
            return [PSCustomObject]@{
                Version = $pinnedVersion
                VisualStudioRoot = $visualStudioRoot
                VcVarsPath = $vcvarsPath
                CompilerPath = $compilerPath
            }
        }
    }

    $available = @($discoveredVersions | Sort-Object)
    $availableText = if ($available.Count -gt 0) { $available -join ", " } else { "none" }
    throw "Required MSVC toolset $pinnedVersion is not installed. Available toolsets: $availableText"
}
