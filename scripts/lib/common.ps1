Set-StrictMode -Version Latest

# Helpers every script uses. Runs on Windows PowerShell 5.1, which is what a stock Windows install has.

# Where the layer keeps its per-user data: logs, backups, staging and the OpenXR layer's files. The folder
# name predates the project's current name and stays so existing installs keep working.
function Get-DvrDataRoot {
    return (Join-Path $env:LOCALAPPDATA "OpenVRDynamicVRS")
}

# Relaunches the calling script elevated when the SteamVR directory is not writable.
# Returns $true when the caller should exit because the elevated copy has run.
function Invoke-DvrElevatedIfNeeded {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][hashtable]$BoundParameters
    )

    $probe = Join-Path $Directory ("dynamic-vrs-write-test-" + [Guid]::NewGuid().ToString("N"))
    try {
        [System.IO.File]::WriteAllText($probe, "")
        Remove-Item -LiteralPath $probe -Force
        return $false
    } catch {
        Write-Verbose "No write access to ${Directory}: $_"
    }

    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Cannot write to $Directory even as administrator."
    }

    $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$ScriptPath`"")
    foreach ($entry in $BoundParameters.GetEnumerator()) {
        if ($entry.Value -is [System.Management.Automation.SwitchParameter]) {
            if ($entry.Value.IsPresent) { $arguments += "-$($entry.Key)" }
        } else {
            $arguments += "-$($entry.Key)"
            $arguments += "`"$($entry.Value)`""
        }
    }
    if (-not $BoundParameters.ContainsKey("PauseAtEnd")) {
        $arguments += "-PauseAtEnd"
    }
    Write-Host "Administrator rights are needed to change $Directory. Asking Windows for permission..."
    $process = Start-Process -FilePath "powershell.exe" -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "The elevated run failed with exit code $($process.ExitCode); see its window for details."
    }
    return $true
}
