<#
.SYNOPSIS
  Configure, build and run the host-side (native) property-based tests for the
  Sony UniLink CD-changer emulator.

.DESCRIPTION
  Spec: unilink-kompendium-alignment, task 1.1.

  cmake / ninja / cl (MSVC) are shipped with Visual Studio but are not on the
  global PATH. This script:
    1. Locates the Visual Studio install (via vswhere).
    2. Imports the MSVC x64 build environment (vcvars64.bat).
    3. Puts the VS-bundled CMake and Ninja on PATH.
    4. Runs: cmake configure (Ninja) -> build -> ctest.

  RapidCheck is fetched automatically by CMake (needs network on first run).

.EXAMPLE
  pwsh -File run_tests.ps1
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Import-VsDevEnv {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found. Is Visual Studio installed?"
    }
    $vsRoot = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $vsRoot) {
        # Fall back to any VS install that has the C++ toolset.
        $vsRoot = & $vswhere -latest -products * -property installationPath
    }
    if (-not $vsRoot) { throw "No Visual Studio installation with C++ tools found." }

    $vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

    Write-Host "Importing MSVC environment from: $vcvars" -ForegroundColor Cyan
    # Run vcvars64.bat in a child cmd and capture the resulting environment.
    $envDump = & "$env:ComSpec" /c "`"$vcvars`" >nul 2>&1 && set"
    foreach ($line in $envDump) {
        if ($line -match '^(.*?)=(.*)$') {
            Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2]
        }
    }

    # Ensure the VS-bundled CMake and Ninja are reachable.
    $cmakeBin = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    $ninjaBin = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
    foreach ($p in @($cmakeBin, $ninjaBin)) {
        if ((Test-Path $p) -and ($env:Path -notlike "*$p*")) {
            $env:Path = "$p;$env:Path"
        }
    }
}

Import-VsDevEnv

$build = Join-Path $scriptDir $BuildDir
if ($Clean -and (Test-Path $build)) {
    Write-Host "Cleaning $build" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $build
}

Write-Host "== CMake configure ==" -ForegroundColor Green
cmake -S $scriptDir -B $build -G Ninja
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

Write-Host "== CMake build ==" -ForegroundColor Green
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

Write-Host "== CTest ==" -ForegroundColor Green
ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)" }

Write-Host "All host-side tests passed." -ForegroundColor Green
