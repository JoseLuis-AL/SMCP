<#
.SYNOPSIS
    Packages an SMCP Release build into a .zip ready to share.

.DESCRIPTION
    Builds the Release configuration (unless -SkipBuild is given), copies the contents of
    build\ninja-release\bin, the app-local Visual C++ runtime, the license, the user
    guide, and a quick-start file for end users, and compresses everything into
    dist\SMCP-<version>-win64.zip. It also writes the SHA256 hash of the package.

.PARAMETER Version
    Package version. By default it is read from project(... VERSION x.y.z) in CMakeLists.txt.

.PARAMETER OutputDir
    Directory that receives the .zip. Defaults to dist\ in the repository root.

.PARAMETER SkipBuild
    Uses the current contents of build\ninja-release\bin without rebuilding.

.EXAMPLE
    .\scripts\package-release.ps1
    .\scripts\package-release.ps1 -SkipBuild -Version 3.0.2
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$OutputDir,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

# ---------------------------------------------------------------------------
# Version and paths
# ---------------------------------------------------------------------------
if (-not $Version) {
    $cmake = Get-Content (Join-Path $repoRoot 'CMakeLists.txt') -Raw
    if ($cmake -notmatch '(?ms)project\s*\(.*?VERSION\s+([0-9]+(?:\.[0-9]+)*)') {
        throw "Could not read the version from CMakeLists.txt; pass -Version explicitly."
    }
    $Version = $Matches[1]
}

if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot 'dist' }

$binDir = Join-Path $repoRoot 'build\ninja-release\bin'
$packageName = "SMCP-$Version-win64"
$stageRoot = Join-Path $repoRoot 'build\package'
$stageDir = Join-Path $stageRoot $packageName
$zipPath = Join-Path $OutputDir "$packageName.zip"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Config Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path (Join-Path $binDir 'SMCP.exe'))) {
    throw "$binDir\SMCP.exe does not exist. Build it first with: .\scripts\build.ps1 -Config Release"
}

# ---------------------------------------------------------------------------
# Prepare the staging directory
# ---------------------------------------------------------------------------
if (Test-Path $stageDir) { Remove-Item -Recurse -Force $stageDir }
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

Write-Host "==> Copying the application from $binDir" -ForegroundColor Green
Copy-Item -Path (Join-Path $binDir '*') -Destination $stageDir -Recurse -Force
# Symbols, import libraries, and CTest executables are not part of the application.
Get-ChildItem -Path $stageDir -Recurse -Include *.pdb, *.ilk, *.exp, *.lib, SMCP_*tests*.exe |
    Remove-Item -Force -ErrorAction SilentlyContinue
$leftovers = Get-ChildItem -Path $stageDir -Recurse -Filter '*test*' -File
if ($leftovers) {
    throw "The package contains test files: $($leftovers.Name -join ', ')"
}

# ---------------------------------------------------------------------------
# Visual C++ runtime next to the executable (no redistributable installer needed)
# ---------------------------------------------------------------------------
$crtNames = @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'concrt140.dll')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$crtDir = $null
if (Test-Path $vswhere) {
    $installs = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
    foreach ($install in @($installs | Sort-Object { [version]$_.installationVersion } -Descending)) {
        $candidate = Get-ChildItem (Join-Path $install.installationPath 'VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT') -Directory -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($candidate) { $crtDir = $candidate.FullName; break }
    }
}

if ($crtDir) {
    Write-Host "==> Copying the Visual C++ runtime from $crtDir" -ForegroundColor Green
    foreach ($name in $crtNames) {
        $source = Join-Path $crtDir $name
        if (Test-Path $source) { Copy-Item $source $stageDir -Force }
    }
} else {
    Write-Warning "The Visual C++ redistributable files were not found; the package will depend on the runtime installed on the target computer."
}

# ---------------------------------------------------------------------------
# Documentation and license
# ---------------------------------------------------------------------------
Copy-Item (Join-Path $repoRoot 'LICENSE') (Join-Path $stageDir 'LICENSE.txt') -Force
# The whole docs directory and README.md at the root keep the relative Markdown links valid.
Copy-Item (Join-Path $repoRoot 'README.md') (Join-Path $stageDir 'README.md') -Force
Copy-Item (Join-Path $repoRoot 'docs') (Join-Path $stageDir 'docs') -Recurse -Force

$spinnaker = Test-Path (Join-Path $stageDir 'Spinnaker_v14*.dll')
$cameraLine = if ($spinnaker) {
    'Direct FLIR/Teledyne camera capture is enabled. It needs a camera with the matching Teledyne device driver installed.'
} else {
    'This package was built without Spinnaker: direct camera capture is disabled and only existing captures can be processed.'
}

$readme = @"
SMCP $Version - Camera-Projector Measuring System (Windows x64)
===============================================================

How to run
----------
1. Extract the whole $packageName folder to a writable location, for example
   your Desktop or Documents. Do not run the program from inside the .zip.
2. Double-click SMCP.exe.
3. Windows SmartScreen may warn about an unknown publisher because the build is
   not code-signed. Choose "More info" > "Run anyway".

Requirements
------------
- Windows 10 or 11, 64-bit.
- A GPU and driver with OpenGL 2.1 or newer for the point-cloud viewer.
- The Visual C++ runtime ships inside this folder; no separate installer is needed.
- $cameraLine
- Optional AI models (Score Denoise, StraightPCF, PointCleanNet) need WSL 2 with
  Ubuntu 22.04 prepared as described in docs\WSL_MODELS_QUICKSTART.md, using the
  scripts-wsl directory of the source repository (https://github.com/JoseLuis-AL/SMCP).
  Without that setup the AI Model selector stays disabled and the rest of SMCP works
  normally.

Keep every file together: SMCP.exe needs the DLLs and the platforms, styles,
imageformats, and iconengines subfolders next to it.

First steps
-----------
See docs\USER_GUIDE.md for the full workflow (workspace layout, Gray-code capture,
decoding, calibration, reconstruction, and the point-cloud editor).
docs\samples\sample_calibration.yml documents the calibration file schema.

Where settings are stored
-------------------------
Window layout and parameters are saved per user in the Windows registry under
HKEY_CURRENT_USER\Software\CENAM\SMCP. Uninstalling is just deleting this folder;
delete that registry key as well to remove the saved settings.

Reporting problems
------------------
Please report the exact step, the message shown, and your Windows version. If the
program does not start at all, note whether a missing-DLL dialog appeared and its
name.

License
-------
BSD 3-Clause. See LICENSE.txt. Qt 5 (LGPLv3), OpenCV 2.4 (BSD), and the Teledyne
Spinnaker runtime are redistributed under their own licenses.
"@
Set-Content -Path (Join-Path $stageDir 'READ_ME_FIRST.txt') -Value $readme -Encoding utf8

# ---------------------------------------------------------------------------
# Compress
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }

Write-Host "==> Compressing $zipPath" -ForegroundColor Green
Compress-Archive -Path $stageDir -DestinationPath $zipPath -CompressionLevel Optimal

$zip = Get-Item $zipPath
$hash = (Get-FileHash $zipPath -Algorithm SHA256).Hash
Set-Content -Path "$zipPath.sha256" -Value "$hash  $($zip.Name)" -Encoding ascii

Write-Host ""
Write-Host "Package : $($zip.FullName)" -ForegroundColor Cyan
Write-Host ("Size    : {0:N1} MB" -f ($zip.Length / 1MB)) -ForegroundColor Cyan
Write-Host "SHA256  : $hash" -ForegroundColor Cyan
