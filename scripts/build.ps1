<#
.SYNOPSIS
    Configura y compila SMCP con CMake desde cualquier consola de PowerShell.

.DESCRIPTION
    Importa el entorno de desarrollador x64 de Visual Studio (cl, cmake, ninja)
    si no esta ya cargado, ejecuta "cmake --preset" y "cmake --build --preset".
    Requiere CMakeUserPresets.json (genéralo con scripts\bootstrap.ps1).

.PARAMETER Config
    Debug (por defecto) o Release.

.PARAMETER Generator
    ninja (por defecto) o vs2026 (genera build\vs2026\SMCP.sln y compila con MSBuild).

.PARAMETER Clean
    Borra el directorio build\<preset> antes de configurar.

.PARAMETER Run
    Lanza el ejecutable al terminar.

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
    Write-Host 'No existe CMakeUserPresets.json. Ejecuta primero: .\scripts\bootstrap.ps1' -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# Entorno de desarrollador de Visual Studio (solo si hace falta)
# ---------------------------------------------------------------------------
function Import-VsDevEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        Write-Host "cl.exe ya esta en PATH; se reutiliza el entorno actual." -ForegroundColor DarkGray
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "No se encontro vswhere.exe. Instala Visual Studio 2026 con la carga 'Desarrollo de escritorio con C++'."
    }

    # Preferir VS 2026 (18.x); si no, la mas reciente con el toolset de C++.
    $installs = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
    if (-not $installs) {
        throw "Visual Studio con el toolset de C++ no encontrado."
    }
    $vs = $installs | Where-Object { $_.installationVersion -like '18.*' } | Select-Object -First 1
    if (-not $vs) {
        $vs = $installs | Sort-Object { [version]$_.installationVersion } -Descending | Select-Object -First 1
    }

    $vsDevCmd = Join-Path $vs.installationPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $vsDevCmd)) {
        throw "No se encontro VsDevCmd.bat en $($vs.installationPath)"
    }

    Write-Host "Importando entorno x64 de: $($vs.displayName) ($($vs.installationVersion))" -ForegroundColor Cyan
    # VsDevCmd.bat invoca vswhere.exe por nombre; lo ponemos en PATH para evitar su aviso.
    $env:PATH = (Split-Path $vswhere) + ';' + $env:PATH
    $envDump = cmd.exe /d /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 -no_logo 2>nul && set"
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "cl.exe sigue sin estar disponible tras importar VsDevCmd."
    }
}

Import-VsDevEnvironment

foreach ($tool in 'cmake.exe', 'ninja.exe') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool no esta en PATH. Instala el componente 'Herramientas de CMake de C++ para Windows' de Visual Studio."
    }
}

# ---------------------------------------------------------------------------
# Configurar y compilar
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
    Write-Host "Limpiando $buildDir" -ForegroundColor Yellow
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
Write-Host "Ejecutable: $exePath" -ForegroundColor Green

if ($Run) {
    Start-Process -FilePath $exePath -WorkingDirectory (Split-Path $exePath)
}
