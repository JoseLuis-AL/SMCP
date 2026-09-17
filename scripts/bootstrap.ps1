<#
.SYNOPSIS
    Detects Qt 5.14.2, OpenCV 2.4.13, and Spinnaker and generates CMakeUserPresets.json.

.DESCRIPTION
    Searches the usual installation locations on C:, D:, and E: (or the paths given
    through parameters and environment variables), uses CMakeUserPresets.example.json
    as a template, replaces its paths, and writes CMakeUserPresets.json to the
    repository root. Missing dependencies are reported.

.PARAMETER Qt
    Qt kit root (for example D:\Qt\Qt5.14.2\5.14.2\msvc2017_64) or its lib\cmake\Qt5 directory.

.PARAMETER OpenCV
    Directory containing OpenCVConfig.cmake (for example D:\OpenCV\opencv\build).

.PARAMETER Spinnaker
    Spinnaker SDK root (for example C:\Program Files\Teledyne\Spinnaker).

.PARAMETER Force
    Overwrites CMakeUserPresets.json when it already exists.

.EXAMPLE
    .\scripts\bootstrap.ps1
    .\scripts\bootstrap.ps1 -Qt C:\Qt\5.14.2\msvc2017_64 -Force
#>
[CmdletBinding()]
param(
    [string]$Qt,
    [string]$OpenCV,
    [string]$Spinnaker,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$examplePath = Join-Path $repoRoot 'CMakeUserPresets.example.json'
$outputPath = Join-Path $repoRoot 'CMakeUserPresets.json'

if ((Test-Path $outputPath) -and -not $Force) {
    Write-Host "$outputPath already exists. Use -Force to regenerate it." -ForegroundColor Yellow
    exit 0
}

$drives = @('C:', 'D:', 'E:')

# ---------------------------------------------------------------------------
# Qt 5.14.2 msvc2017_64
# ---------------------------------------------------------------------------
function Find-Qt {
    param([string]$Hint)

    $candidates = @()
    if ($Hint) { $candidates += $Hint }
    if ($env:Qt5_DIR) { $candidates += $env:Qt5_DIR }
    if ($env:QTDIR) { $candidates += $env:QTDIR }
    foreach ($d in $drives) {
        $candidates += "$d\Qt\Qt5.14.2\5.14.2\msvc2017_64"
        $candidates += "$d\Qt\5.14.2\msvc2017_64"
        $candidates += "$d\Qt5.14.2\5.14.2\msvc2017_64"
        # Installations with a different root directory name: <drive>\Qt*\[...\]5.14.2\msvc2017_64
        foreach ($pattern in @("$d\Qt*\5.14.2\msvc2017_64", "$d\Qt*\*\5.14.2\msvc2017_64")) {
            Resolve-Path -Path $pattern -ErrorAction SilentlyContinue | ForEach-Object { $candidates += $_.Path }
        }
    }

    foreach ($c in $candidates) {
        if (-not $c) { continue }
        foreach ($p in @($c, (Join-Path $c 'lib\cmake\Qt5'))) {
            if (Test-Path (Join-Path $p 'Qt5Config.cmake')) {
                return (Resolve-Path $p).Path
            }
        }
    }
    return $null
}

# ---------------------------------------------------------------------------
# OpenCV 2.4.13 (Windows package: build\OpenCVConfig.cmake + x64\vc14)
# ---------------------------------------------------------------------------
function Find-OpenCV {
    param([string]$Hint)

    $candidates = @()
    if ($Hint) { $candidates += $Hint }
    if ($env:OpenCV_DIR) { $candidates += $env:OpenCV_DIR }
    if ($env:OPENCV_DIR) { $candidates += $env:OPENCV_DIR }
    foreach ($d in $drives) {
        $candidates += "$d\OpenCV\opencv\build"
        $candidates += "$d\opencv\build"
        $candidates += "$d\OpenCV\build"
        $candidates += "$d\opencv-2.4.13\build"
        $candidates += "$d\Program Files\opencv\build"
    }

    foreach ($c in $candidates) {
        if (-not $c) { continue }
        if ((Test-Path (Join-Path $c 'OpenCVConfig.cmake')) -and (Test-Path (Join-Path $c 'x64\vc14\lib'))) {
            return (Resolve-Path $c).Path
        }
    }
    return $null
}

# ---------------------------------------------------------------------------
# Spinnaker SDK
# ---------------------------------------------------------------------------
function Find-Spinnaker {
    param([string]$Hint)

    $candidates = @()
    if ($Hint) { $candidates += $Hint }
    if ($env:SPINNAKER_DIR) { $candidates += $env:SPINNAKER_DIR }
    foreach ($d in $drives) {
        $candidates += "$d\Program Files\Teledyne\Spinnaker"
        $candidates += "$d\Program Files\FLIR Systems\Spinnaker"
        $candidates += "$d\Spinnaker"
    }

    foreach ($c in $candidates) {
        if (-not $c) { continue }
        $hasHeader = Test-Path (Join-Path $c 'include\Spinnaker.h')
        $hasLib = (Test-Path (Join-Path $c 'lib64\vs2017\Spinnaker_v141.lib')) -or
                  (Test-Path (Join-Path $c 'lib64\vs2015\Spinnaker_v140.lib'))
        if ($hasHeader -and $hasLib) {
            return (Resolve-Path $c).Path
        }
    }
    return $null
}

function Format-CMakePath {
    param([string]$Path)
    if (-not $Path) { return $null }
    return ($Path -replace '\\', '/').TrimEnd('/')
}

Write-Host 'Searching for dependencies...' -ForegroundColor Cyan
$qtDir = Find-Qt -Hint $Qt
$openCvDir = Find-OpenCV -Hint $OpenCV
$spinnakerDir = Find-Spinnaker -Hint $Spinnaker

$missing = @()
if ($qtDir) { Write-Host "  [OK] Qt 5.14.2   : $qtDir" -ForegroundColor Green }
else { Write-Host '  [--] Qt 5.14.2   : not found (msvc2017_64 kit)' -ForegroundColor Red; $missing += 'Qt' }

if ($openCvDir) { Write-Host "  [OK] OpenCV 2.4  : $openCvDir" -ForegroundColor Green }
else { Write-Host '  [--] OpenCV 2.4  : not found (build directory with x64\vc14)' -ForegroundColor Red; $missing += 'OpenCV' }

if ($spinnakerDir) { Write-Host "  [OK] Spinnaker   : $spinnakerDir" -ForegroundColor Green }
else { Write-Host '  [--] Spinnaker   : not found; the preset will use SMCP_WITH_SPINNAKER=OFF' -ForegroundColor Yellow }

# ---------------------------------------------------------------------------
# Generate CMakeUserPresets.json from the template
# ---------------------------------------------------------------------------
# Values are replaced in the template text so its formatting is preserved.
$json = Get-Content -Raw -Path $examplePath
if ($json -notmatch '"name":\s*"local-paths"') {
    throw "The template $examplePath does not contain the hidden 'local-paths' preset."
}

$values = @{
    'Qt5_DIR'             = if ($qtDir) { Format-CMakePath $qtDir } else { 'PATH/TO/Qt/5.14.2/msvc2017_64/lib/cmake/Qt5' }
    'OpenCV_DIR'          = if ($openCvDir) { Format-CMakePath $openCvDir } else { 'PATH/TO/opencv/build' }
    'SPINNAKER_DIR'       = if ($spinnakerDir) { Format-CMakePath $spinnakerDir } else { '' }
    'SMCP_WITH_SPINNAKER' = if ($spinnakerDir) { 'ON' } else { 'OFF' }
}
foreach ($key in $values.Keys) {
    $pattern = '("' + [regex]::Escape($key) + '"\s*:\s*)"[^"]*"'
    $replacement = '${1}"' + $values[$key] + '"'
    $json = (New-Object System.Text.RegularExpressions.Regex $pattern).Replace($json, $replacement, 1)
}
$json = $json -replace '"description":\s*"Machine-specific paths[^"]*"', '"description": "Machine-specific paths (generated by scripts/bootstrap.ps1)."'

[System.IO.File]::WriteAllText($outputPath, $json, (New-Object System.Text.UTF8Encoding($false)))

Write-Host ''
Write-Host "Wrote $outputPath" -ForegroundColor Green

if ($missing.Count -gt 0) {
    Write-Host ''
    Write-Host "Required dependencies are missing: $($missing -join ', ')." -ForegroundColor Red
    Write-Host 'Install them, or edit the paths in CMakeUserPresets.json, and build again.' -ForegroundColor Red
    Write-Host 'See docs/BUILD.md for the required versions and locations.' -ForegroundColor Red
    exit 2
}

Write-Host ''
Write-Host 'Next step:' -ForegroundColor Cyan
Write-Host '  .\scripts\build.ps1 -Config Debug     (or open the folder in Visual Studio 2026)'
