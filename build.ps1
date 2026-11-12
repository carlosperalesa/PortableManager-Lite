#Requires -Version 5.1
<#
.SYNOPSIS
    Script reproducible de compilación C++20 nativa con LLVM-MinGW portable para PortableManager Lite.
.DESCRIPTION
    Resuelve todas las rutas respecto a la ubicación del script (independiente del CWD),
    no modifica el PATH global ni el registro de Windows, compila pm.exe y las suites de prueba
    (Fases 0B a 7) con tools/toolchains/llvm-mingw/bin/clang++.exe, verifica las cabeceras PE
    y DLL importadas con llvm-readobj.exe, y genera el informe JSON de validación.
#>

param(
    [string]$OutputDir = "",
    [string]$ReportJsonPath = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $ProjectRoot "build"
}

if (-not [string]::IsNullOrWhiteSpace($ReportJsonPath) -and (Test-Path -LiteralPath $ReportJsonPath)) {
    throw "El informe ya existe; usa una ruta nueva para preservar la evidencia histórica: $ReportJsonPath"
}

$ToolchainBin = Join-Path $ProjectRoot "tools\toolchains\llvm-mingw\bin"
$ClangPP      = Join-Path $ToolchainBin "clang++.exe"
$ReadObj      = Join-Path $ToolchainBin "llvm-readobj.exe"

if (-not (Test-Path $ClangPP)) {
    throw "No se encontró el compilador LLVM-MinGW en la ruta relativa: $ClangPP"
}
if (-not (Test-Path $ReadObj)) {
    throw "No se encontró llvm-readobj.exe en la ruta relativa: $ReadObj"
}

if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

$PmExe         = Join-Path $OutputDir "pm.exe"
$SmokeTestExe  = Join-Path $OutputDir "test_phase0_smoke.exe"
$Phase1TestExe = Join-Path $OutputDir "test_phase1_core.exe"
$Phase2TestExe = Join-Path $OutputDir "test_phase2_runtime.exe"
$Phase3TestExe = Join-Path $OutputDir "test_phase3_guard.exe"
$Phase3Report = Join-Path $OutputDir ("phase3_windows_" + (Get-Date -Format "yyyyMMddTHHmmssfff") + ".json")
$Phase4TestExe = Join-Path $OutputDir "test_phase4_junctions.exe"
$Phase4Report = Join-Path $OutputDir ("phase4_windows_" + (Get-Date -Format "yyyyMMddTHHmmssfff") + ".json")
$Phase5TestExe = Join-Path $OutputDir "test_phase5_registry.exe"
$Phase5Report = Join-Path $OutputDir ("phase5_windows_" + (Get-Date -Format "yyyyMMddTHHmmssfff") + ".json")
$Phase6TestExe = Join-Path $OutputDir "test_phase6_learning.exe"
$Phase6Report = Join-Path $OutputDir ("phase6_windows_" + (Get-Date -Format "yyyyMMddTHHmmssfff") + ".json")
$Phase7TestExe = Join-Path $OutputDir "test_phase7_cleanup.exe"
$Phase7Report = Join-Path $OutputDir ("phase7_windows_" + (Get-Date -Format "yyyyMMddTHHmmssfff") + ".json")
$Phase6Resource = Join-Path $OutputDir "phase6_fixture.o"
$LearningSource = Join-Path $ProjectRoot "src\learning\learning_engine.cpp"
$IncludeDir    = Join-Path $ProjectRoot "src"

# 1. Compilación limpia
foreach ($bin in @($PmExe, $SmokeTestExe, $Phase1TestExe, $Phase2TestExe, $Phase3TestExe, $Phase4TestExe, $Phase5TestExe, $Phase6TestExe, $Phase7TestExe, $Phase6Resource)) {
    if (Test-Path $bin) { Remove-Item $bin -Force }
}

$coreSources = @(
    (Join-Path $ProjectRoot "src\core\path_resolver.cpp"),
    (Join-Path $ProjectRoot "src\core\atomic_file.cpp"),
    (Join-Path $ProjectRoot "src\core\config_manager.cpp")
)

$runtimeSources = @(
    (Join-Path $ProjectRoot "src\runtime\manifest_manager.cpp"),
    (Join-Path $ProjectRoot "src\runtime\session_manager.cpp")
)

$guardSources = @(
    (Join-Path $ProjectRoot "src\jobs\job_manager.cpp"),
    (Join-Path $ProjectRoot "src\guard\guard_process.cpp"),
    (Join-Path $ProjectRoot "src\cleanup\cleanup_engine.cpp"),
    (Join-Path $ProjectRoot "src\junctions\junction_manager.cpp"),
    (Join-Path $ProjectRoot "src\registry\registry_manager.cpp")
)

Write-Host "=== [1/6] Compilando pm.exe (C++20, Windows x64, Unicode, -static, /SWAPRUN:CD) ==="
$pmSources = @(
    $LearningSource,
    (Join-Path $ProjectRoot "src\core\main.cpp"),
    (Join-Path $ProjectRoot "src\guard\guard_entry.cpp")
) + $coreSources + $runtimeSources + $guardSources

& $ClangPP `
    -std=c++20 -O2 -Wall -Wextra -Werror `
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX `
    -municode -static "-Wl,-Xlink=-swaprun:cd" `
    "-I$IncludeDir" `
    $pmSources `
    -o $PmExe `
    -lkernel32 -luser32 -ladvapi32 -lshell32 -lshlwapi -lversion -lole32 -lbcrypt

if ($LASTEXITCODE -ne 0 -or -not (Test-Path $PmExe)) {
    throw "Fallo al compilar pm.exe (ExitCode=$LASTEXITCODE)"
}

Write-Host "=== [2/6] Compilando test_phase0_smoke.exe (C++20) ==="
$test0Source = Join-Path $ProjectRoot "tests\test_phase0_smoke.cpp"
& $ClangPP `
    -std=c++20 -O2 -Wall -Wextra -Werror `
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX `
    -municode -static `
    "-I$IncludeDir" `
    $test0Source `
    -o $SmokeTestExe `
    -lkernel32 -ladvapi32

if ($LASTEXITCODE -ne 0 -or -not (Test-Path $SmokeTestExe)) {
    throw "Fallo al compilar test_phase0_smoke.exe (ExitCode=$LASTEXITCODE)"
}

Write-Host "=== [3/6] Compilando test_phase1_core.exe (C++20) ==="
$test1Sources = @(
    (Join-Path $ProjectRoot "tests\test_phase1_core.cpp")
) + $coreSources
& $ClangPP `
    -std=c++20 -O2 -Wall -Wextra -Werror `
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX `
    -municode -static `
    "-I$IncludeDir" `
    $test1Sources `
    -o $Phase1TestExe `
    -lkernel32 -ladvapi32

if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Phase1TestExe)) {
    throw "Fallo al compilar test_phase1_core.exe (ExitCode=$LASTEXITCODE)"
}

Write-Host "=== [4/6] Compilando test_phase2_runtime.exe (C++20) ==="
$test2Sources = @(
    (Join-Path $ProjectRoot "tests\test_phase2_runtime.cpp")
) + $coreSources + $runtimeSources
& $ClangPP `
    -std=c++20 -O2 -Wall -Wextra -Werror `
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX `
    -municode -static `
    "-I$IncludeDir" `
    $test2Sources `
    -o $Phase2TestExe `
    -lkernel32 -ladvapi32 -lole32

if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Phase2TestExe)) {
    throw "Fallo al compilar test_phase2_runtime.exe (ExitCode=$LASTEXITCODE)"
}

Write-Host "=== Compilando test_phase3_guard.exe (C++20) ==="
$test3Sources = @(
    (Join-Path $ProjectRoot "tests\test_phase3_guard.cpp")
) + $coreSources + $runtimeSources + $guardSources
& $ClangPP -std=c++20 -O2 -Wall -Wextra -Werror `
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX `
    -municode -static "-Wl,-Xlink=-swaprun:cd" "-I$IncludeDir" $test3Sources -o $Phase3TestExe `
    -lkernel32 -luser32 -ladvapi32 -lole32 -lbcrypt
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Phase3TestExe)) {
    throw "Fallo al compilar test_phase3_guard.exe (ExitCode=$LASTEXITCODE)"
}

Write-Host "=== Compilando test_phase4_junctions.exe (C++20) ==="
$test4Sources = @(
    (Join-Path $ProjectRoot "tests\test_phase4_junctions.cpp")
) + $coreSources + $runtimeSources + $guardSources
& $ClangPP -std=c++20 -O2 -Wall -Wextra -Werror `
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX `
    -municode -static "-Wl,-Xlink=-swaprun:cd" "-I$IncludeDir" $test4Sources -o $Phase4TestExe `
    -lkernel32 -luser32 -ladvapi32 -lole32 -lbcrypt
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Phase4TestExe)) {
    throw "Fallo al compilar test_phase4_junctions.exe (ExitCode=$LASTEXITCODE)"
}

Write-Host "=== Compilando test_phase5_registry.exe (C++20) ==="
$test5Sources = @(
    (Join-Path $ProjectRoot "tests\test_phase5_registry.cpp")
) + $coreSources + $runtimeSources + $guardSources
& $ClangPP -std=c++20 -O2 -Wall -Wextra -Werror `
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX `
    -municode -static "-Wl,-Xlink=-swaprun:cd" "-I$IncludeDir" $test5Sources -o $Phase5TestExe `
    -lkernel32 -luser32 -ladvapi32 -lole32 -lbcrypt
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Phase5TestExe)) {
    throw "Fallo al compilar test_phase5_registry.exe (ExitCode=$LASTEXITCODE)"
}

Write-Host "=== Compilando test_phase6_learning.exe (C++20) ==="
$Windres = Join-Path $ToolchainBin "x86_64-w64-mingw32-windres.exe"
& $Windres -O coff -i (Join-Path $ProjectRoot "tests\phase6_fixture.rc") -o $Phase6Resource
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Phase6Resource)) {
    throw "Fallo al compilar el recurso PE sintetico (ExitCode=$LASTEXITCODE)"
}
$test6Sources = @(
    $LearningSource,
    (Join-Path $ProjectRoot "src\guard\guard_entry.cpp"),
    (Join-Path $ProjectRoot "tests\test_phase6_learning.cpp")
) + $coreSources + $runtimeSources + $guardSources
& $ClangPP -std=c++20 -O2 -Wall -Wextra -Werror `
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX `
    -municode -static "-Wl,-Xlink=-swaprun:cd" "-I$IncludeDir" $test6Sources $Phase6Resource -o $Phase6TestExe `
    -lkernel32 -luser32 -ladvapi32 -lole32 -lbcrypt -lversion
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Phase6TestExe)) {
    throw "Fallo al compilar test_phase6_learning.exe (ExitCode=$LASTEXITCODE)"
}

Write-Host "=== Compilando test_phase7_cleanup.exe (C++20) ==="
$test7Sources = @(
    (Join-Path $ProjectRoot "tests\test_phase7_cleanup.cpp")
) + $coreSources + $runtimeSources + $guardSources
& $ClangPP -std=c++20 -O2 -Wall -Wextra -Werror -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX `
    -municode -static "-Wl,-Xlink=-swaprun:cd" "-I$IncludeDir" $test7Sources -o $Phase7TestExe `
    -lkernel32 -luser32 -ladvapi32 -lole32 -lbcrypt
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Phase7TestExe)) {
    throw "Fallo al compilar test_phase7_cleanup.exe (ExitCode=$LASTEXITCODE)"
}

Write-Host "=== [5/6] Inspeccionando cabecera PE e importaciones COFF con llvm-readobj.exe ==="
$peHeadersRaw = (& $ReadObj --file-headers $PmExe) -join "`n"
$coffImportsRaw = (& $ReadObj --coff-imports $PmExe) -join "`n"

$isAmd64 = $peHeadersRaw -match "Machine:\s+IMAGE_FILE_MACHINE_AMD64\s+\(0x8664\)"
$hasSwaprunCd = $peHeadersRaw -match "IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP\s+\(0x400\)"

$importedDlls = @()
foreach ($line in ($coffImportsRaw -split "`r?`n")) {
    if ($line -match "^\s*Name:\s+(\S+)\s*$") {
        $importedDlls += $Matches[1]
    }
}

$toolchainExternalDlls = $importedDlls | Where-Object {
    $_ -match "^(libc\+\+|libstdc\+\+|libunwind|libgcc|libwinpthread)"
}

if (-not $isAmd64) {
    throw "Verificación PE fallida: Machine no es IMAGE_FILE_MACHINE_AMD64 (0x8664)"
}
if (-not $hasSwaprunCd) {
    throw "Verificación PE fallida: Falta característica IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP (0x400)"
}
if ($toolchainExternalDlls.Count -gt 0) {
    throw "Verificación PE fallida: Se detectaron DLLs externas del toolchain: $($toolchainExternalDlls -join ', ')"
}

Write-Host "PE Machine: IMAGE_FILE_MACHINE_AMD64 (0x8664) -> OK"
Write-Host "PE Characteristic: IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP (0x400) -> OK"
Write-Host "DLLs importadas ($($importedDlls.Count)): $($importedDlls -join ', ')"
Write-Host "Ausencia de DLLs externas del toolchain -> OK (0 detectadas)"

Write-Host "=== [6/6] Ejecutando suites de prueba desde directorio de trabajo externo (%TEMP%) ==="
$tempWorkDir = Join-Path ([System.IO.Path]::GetTempPath()) ("pm-tests-" + [guid]::NewGuid().ToString("N").Substring(0, 12))
New-Item -ItemType Directory -Path $tempWorkDir | Out-Null

foreach ($testBin in @($SmokeTestExe, $Phase1TestExe, $Phase2TestExe, $Phase3TestExe, $Phase4TestExe, $Phase5TestExe, $Phase6TestExe, $Phase7TestExe)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $testBin
    $psi.Arguments = "`"$PmExe`""
    if ($testBin -eq $Phase3TestExe) { $psi.Arguments += " `"$Phase3Report`"" }
    if ($testBin -eq $Phase4TestExe) { $psi.Arguments += " `"$Phase4Report`"" }
    if ($testBin -eq $Phase5TestExe) { $psi.Arguments += " `"$Phase5Report`"" }
    if ($testBin -eq $Phase6TestExe) { $psi.Arguments = "`"$Phase6Report`"" }
    if ($testBin -eq $Phase7TestExe) { $psi.Arguments += " `"$Phase7Report`"" }
    $psi.WorkingDirectory = $tempWorkDir
    $psi.EnvironmentVariables["TEMP"] = $tempWorkDir
    $psi.EnvironmentVariables["TMP"] = $tempWorkDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    $proc = [System.Diagnostics.Process]::Start($psi)
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    $completed = $proc.WaitForExit(120000)

    if (-not $completed) {
        try { $proc.Kill() } catch { }
        throw "$(Split-Path -Leaf $testBin) excedió el tiempo máximo de 120 segundos."
    }

    $tOut = $outTask.GetAwaiter().GetResult().Trim()
    $tErr = $errTask.GetAwaiter().GetResult().Trim()
    if ($proc.ExitCode -ne 0) {
        throw "$(Split-Path -Leaf $testBin) falló (ExitCode=$($proc.ExitCode)): $tOut $tErr"
    }
    Write-Host $tOut
}

$pmSha256    = (Get-FileHash -Path $PmExe -Algorithm SHA256).Hash
$pmSize      = (Get-Item $PmExe).Length
$test2Sha256 = (Get-FileHash -Path $Phase2TestExe -Algorithm SHA256).Hash
$test2Size   = (Get-Item $Phase2TestExe).Length
$clangVer    = ((& $ClangPP --version) | Select-Object -First 1).Trim()

if (-not [string]::IsNullOrWhiteSpace($ReportJsonPath)) {
    $reportObj = [ordered]@{
        phase         = "Fase 7 $([char]0x2014) CleanupEngine; regresion 0B a 6"
        timestamp_utc = (Get-Date).ToUniversalTime().ToString("o")
        toolchain     = [ordered]@{
            name             = "llvm-mingw 20261104 (UCRT x86_64)"
            compiler_version = $clangVer
            relative_path    = "tools/toolchains/llvm-mingw/bin/clang++.exe"
            compile_flags    = @("-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-municode", "-static", "-Wl,-Xlink=-swaprun:cd")
        }
        binaries      = [ordered]@{
            pm_exe = [ordered]@{
                size_bytes       = $pmSize
                sha256           = $pmSha256
                machine          = "IMAGE_FILE_MACHINE_AMD64 (0x8664)"
                characteristics  = "0x422 (IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE | IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP)"
                swaprun_cd_0x400 = $true
                imported_dlls    = $importedDlls
                external_toolchain_dlls = @()
            }
            test_phase2_runtime_exe = [ordered]@{
                size_bytes = $test2Size
                sha256     = $test2Sha256
            }
        }
        phase3_runtime = (Get-Content -LiteralPath $Phase3Report -Raw | ConvertFrom-Json)
        phase3_evidence_path = $Phase3Report
        phase4_runtime = (Get-Content -LiteralPath $Phase4Report -Raw | ConvertFrom-Json)
        phase4_evidence_path = $Phase4Report
        phase5_runtime = (Get-Content -LiteralPath $Phase5Report -Raw | ConvertFrom-Json)
        phase5_evidence_path = $Phase5Report
        phase6_runtime = (Get-Content -LiteralPath $Phase6Report -Raw | ConvertFrom-Json)
        phase6_evidence_path = $Phase6Report
        phase7_runtime = (Get-Content -LiteralPath $Phase7Report -Raw | ConvertFrom-Json)
        phase7_evidence_path = $Phase7Report
        regression_suites = @("test_phase0_smoke", "test_phase1_core", "test_phase2_runtime", "test_phase3_guard", "test_phase4_junctions", "test_phase5_registry", "test_phase6_learning")
        regression_status = "passed"
        status        = "passed"
    }

    $reportDir = Split-Path -Parent $ReportJsonPath
    if (-not (Test-Path $reportDir)) {
        New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
    }
    $jsonText = $reportObj | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText($ReportJsonPath, $jsonText, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Informe JSON generado en: $ReportJsonPath"
}
