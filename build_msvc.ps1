#Requires -Version 5.1
<#
.SYNOPSIS
    Script de compilación nativa MSVC para PortableManager Lite (Fase 0+).
.DESCRIPTION
    Detecta CMake o MSVC (cl.exe + link.exe + Windows SDK) en el sistema y compila
    pm.exe (x64, C++20, /MT, /SWAPRUN:CD) y los tests correspondientes.
    No instala herramientas automáticamente (Sección 4).
#>

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "=== PortableManager Lite - Verificacion de Toolchain Nativo C++ (x64) ==="

# 1. Buscar vswhere.exe
$vswherePaths = @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
)
$vswhere = $vswherePaths | Where-Object { Test-Path $_ } | Select-Object -First 1

$vcvars64 = $null
if ($vswhere) {
    $vsInstallDir = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsInstallDir) {
        $candidate = Join-Path $vsInstallDir "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $candidate) {
            $vcvars64 = $candidate
        }
    }
}

# 2. Buscar vcvars64.bat en rutas estandar si vswhere no existe
if (-not $vcvars64) {
    $standardRoots = @(
        "${env:ProgramFiles}\Microsoft Visual Studio",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
    )
    foreach ($root in $standardRoots) {
        if (Test-Path $root) {
            $found = Get-ChildItem -Path $root -Filter "vcvars64.bat" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($found) {
                $vcvars64 = $found.FullName
                break
            }
        }
    }
}

# 3. Verificar cl.exe en PATH
$clCmd = Get-Command cl.exe -ErrorAction SilentlyContinue
$cmakeCmd = Get-Command cmake.exe -ErrorAction SilentlyContinue

if (-not $clCmd -and -not $vcvars64) {
    Write-Error @"
[BLOQUEO DE COMPILACION - FASE 0]
No se encontro el compilador MSVC (cl.exe), Visual Studio Build Tools ni el Windows SDK (Windows.h / kernel32.lib) en el entorno actual.
Conforme a la Seccion 4 ('No instalar herramientas automaticamente') y Seccion 5 ('Detenerse si no se consigue compilar'), el proceso se detiene.

Para habilitar la compilacion nativa C++20/Win32 x64 se requiere:
  - MSVC v142/v143 (Visual Studio 2019/2022 Build Tools - componente 'VC++ x64/x86 build tools')
  - Windows 10/11 SDK (Windows.h, kernel32.lib, advapi32.lib, version.lib, user32.lib, shell32.lib)
  - Opcionalmente: CMake >= 3.20
"@
    exit 1
}

$BuildDir = Join-Path $ProjectRoot "build"
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

if ($cmakeCmd) {
    Write-Host "Compilando mediante CMake..."
    & cmake -S $ProjectRoot -B $BuildDir -A x64
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & cmake --build $BuildDir --config Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & ctest --test-dir $BuildDir -C Release --output-on-failure
    exit $LASTEXITCODE
} else {
    Write-Host "Compilando directamente con MSVC (cl.exe)..."
    $compileCmd = @"
call "$vcvars64" >nul
cl.exe /nologo /std:c++20 /MT /O2 /W4 /WX /utf-8 /permissive- /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"$ProjectRoot\src" "$ProjectRoot\src\core\main.cpp" "$ProjectRoot\src\guard\guard_entry.cpp" /Fe:"$BuildDir\pm.exe" /Fo:"$BuildDir\" /link /SWAPRUN:CD kernel32.lib user32.lib advapi32.lib shell32.lib shlwapi.lib version.lib ole32.lib
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c++20 /MT /O2 /W4 /WX /utf-8 /permissive- /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"$ProjectRoot\src" "$ProjectRoot\tests\test_phase0_smoke.cpp" /Fe:"$BuildDir\test_phase0_smoke.exe" /Fo:"$BuildDir\" /link kernel32.lib advapi32.lib
if errorlevel 1 exit /b 1
"$BuildDir\test_phase0_smoke.exe" "$BuildDir\pm.exe"
"@
    $batFile = Join-Path $BuildDir "run_msvc_build.bat"
    Set-Content -Path $batFile -Value $compileCmd -Encoding ASCII
    & cmd.exe /c "`"$batFile`""
    exit $LASTEXITCODE
}
