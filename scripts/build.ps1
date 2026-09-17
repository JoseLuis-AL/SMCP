<#
.SYNOPSIS
    Configures and builds SMCP with CMake from any PowerShell console.

.DESCRIPTION
    Imports the Visual Studio x64 developer environment (cl, cmake, ninja) when it
    is not already loaded, then runs "cmake --preset" and "cmake --build --preset".
    Requires CMakeUserPresets.json (generate it with scripts\bootstrap.ps1).

.PARAMETER Config
    Debug (default) or Release.

.PARAMETER Generator
    ninja (default) or vs2026 (generates build\vs2026\SMCP.slnx and builds with MSBuild).

.PARAMETER Clean
    Deletes the build\<preset> directory before configuring.

.PARAMETER Run
    Starts the executable when the build finishes.

.EXAMPLE
    .\scripts\build.ps1
    .\scripts\build.ps1 -Config Release -Run
    .\scripts\build.ps1 -Generator vs2026 -Config Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',

    [ValidateSet('ninja', 'vs2026')]
    [string]$Generator = 'ninja',

    [switch]$Clean,
    [switch]$Run
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

if (-not (Test-Path (Join-Path $repoRoot 'CMakeUserPresets.json'))) {
    Write-Host 'CMakeUserPresets.json does not exist. Run .\scripts\bootstrap.ps1 first.' -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# Visual Studio developer environment (only when needed)
# ---------------------------------------------------------------------------
function Import-VsDevEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        Write-Host "cl.exe is already in PATH; reusing the current environment." -ForegroundColor DarkGray
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe was not found. Install Visual Studio 2026 with the 'Desktop development with C++' workload."
    }

    # Prefer VS 2026 (18.x); otherwise use the newest installation with the C++ toolset.
    $installs = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
    if (-not $installs) {
        throw "No Visual Studio installation with the C++ toolset was found."
    }
    $vs = $installs | Where-Object { $_.installationVersion -like '18.*' } | Select-Object -First 1
    if (-not $vs) {
        $vs = $installs | Sort-Object { [version]$_.installationVersion } -Descending | Select-Object -First 1
    }

    $vsDevCmd = Join-Path $vs.installationPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $vsDevCmd)) {
        throw "VsDevCmd.bat was not found in $($vs.installationPath)"
    }

    Write-Host "Importing the x64 environment from: $($vs.displayName) ($($vs.installationVersion))" -ForegroundColor Cyan
    # VsDevCmd.bat calls vswhere.exe by name; adding it to PATH avoids its warning.
    $env:PATH = (Split-Path $vswhere) + ';' + $env:PATH
    $envDump = cmd.exe /d /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 -no_logo 2>nul && set"
    # Some hosts expose both PATH and Path. cmd.exe prints both, but VsDevCmd updates only
    # PATH; importing the later stale alias would silently discard the compiler directory.
    $importedNames = @{}
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$' -and -not $importedNames.ContainsKey($Matches[1])) {
            [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
            $importedNames[$Matches[1]] = $true
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "cl.exe is still unavailable after importing VsDevCmd."
    }
}

Import-VsDevEnvironment

foreach ($tool in 'cmake.exe', 'ninja.exe') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is not in PATH. Install the Visual Studio component 'C++ CMake tools for Windows'."
    }
}

# ---------------------------------------------------------------------------
# Configure and build
# ---------------------------------------------------------------------------
if ($Generator -eq 'ninja') {
    $configurePreset = "ninja-$($Config.ToLower())"
    $buildPreset = $configurePreset
} else {
    $configurePreset = 'vs2026'
    $buildPreset = "vs2026-$($Config.ToLower())"
}

$buildDir = Join-Path $repoRoot "build\$configurePreset"
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

Write-Host "==> cmake --preset $configurePreset" -ForegroundColor Green
& cmake --preset $configurePreset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> cmake --build --preset $buildPreset" -ForegroundColor Green
& cmake --build --preset $buildPreset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$exeName = if ($Config -eq 'Debug') { 'SMCP_d.exe' } else { 'SMCP.exe' }
$exePath = Join-Path $buildDir "bin\$exeName"
Write-Host "Executable: $exePath" -ForegroundColor Green

if ($Run) {
    Start-Process -FilePath $exePath -WorkingDirectory (Split-Path $exePath)
}
