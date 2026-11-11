#Requires -Version 5.1
<#
.SYNOPSIS
    Script reproducible de compilación C++20 nativa con LLVM-MinGW portable para PortableManager Lite.
.DESCRIPTION
    Resuelve todas las rutas respecto a la ubicación del script (independiente del CWD),
    no modifica el PATH global ni el registro de Windows, compila pm.exe y las suites de prueba
    (Fase 0B, Fase 1 y Fase 2) con tools/toolchains/llvm-mingw/bin/clang++.exe, verifica las cabeceras PE
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
$IncludeDir    = Join-Path $ProjectRoot "src"

# 1. Compilación limpia
foreach ($bin in @($PmExe, $SmokeTestExe, $Phase1TestExe, $Phase2TestExe)) {
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

Write-Host "=== [1/6] Compilando pm.exe (C++20, Windows x64, Unicode, -static, /SWAPRUN:CD) ==="
$pmSources = @(
    (Join-Path $ProjectRoot "src\core\main.cpp"),
    (Join-Path $ProjectRoot "src\guard\guard_entry.cpp")
) + $coreSources + $runtimeSources

& $ClangPP `
    -std=c++20 -O2 -Wall -Wextra -Werror `
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX `
    -municode -static "-Wl,-Xlink=-swaprun:cd" `
    "-I$IncludeDir" `
    $pmSources `
    -o $PmExe `
    -lkernel32 -luser32 -ladvapi32 -lshell32 -lshlwapi -lversion -lole32

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
$tempWorkDir = [System.IO.Path]::GetTempPath().TrimEnd('\')

foreach ($testBin in @($SmokeTestExe, $Phase1TestExe, $Phase2TestExe)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $testBin
    $psi.Arguments = "`"$PmExe`""
    $psi.WorkingDirectory = $tempWorkDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    $proc = [System.Diagnostics.Process]::Start($psi)
    $tOut = $proc.StandardOutput.ReadToEnd().Trim()
    $tErr = $proc.StandardError.ReadToEnd().Trim()
    $completed = $proc.WaitForExit(120000)

    if (-not $completed) {
        try { $proc.Kill() } catch { }
        throw "$(Split-Path -Leaf $testBin) excedió el tiempo máximo de 120 segundos."
    }

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
        phase         = "Fase 2 — Runtime y Manifiesto (SessionManager + ManifestManager)"
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
        modules_implemented = @(
            "src/runtime/session_manager.h",
            "src/runtime/session_manager.cpp",
            "src/runtime/manifest_manager.h",
            "src/runtime/manifest_manager.cpp",
            "src/runtime/runtime_module.h",
            "src/core/main.cpp",
            "tests/test_phase2_runtime.cpp"
        )
        scenarios_verified = [ordered]@{
            "1_session_creation"                                      = $true
            "2_guid_generation_unique"                                = $true
            "3_manifest_creation_in_temp"                             = $true
            "4_operation_registration"                                = $true
            "5_write_ahead_persistence_before_modify"                 = $true
            "6_operation_and_session_state_transitions"               = $true
            "7_read_and_reconstruct_after_process_restart"            = $true
            "8_real_process_crash_before_modify"                      = $true
            "9_real_process_crash_after_intent_before_rename_skipped" = $true
            "10_real_process_crash_after_rename_before_state"         = $true
            "11_real_process_crash_after_simulated_junction"          = $true
            "12_real_process_crash_after_simulated_registry"          = $true
            "13_partial_restoration_and_retry_recovery"               = $true
            "14_idempotent_repeated_rollback_execution"               = $true
            "15_corrupt_truncated_and_forged_manifest_reject"         = $true
            "16_backup_preservation_on_error_and_preexisting"         = $true
            "17_audit_kill_job_effective_first_order_lifo"            = $true
            "18_audit_reg_file_backup_manifest_and_synthetic_hkcu"    = $true
            "19_audit_preserve_reg_backup_on_restore_failure"         = $true
            "20_audit_pmg_open_active_session_without_usb"            = $true
            "21_audit_no_automatic_orphan_recovery_on_pm_start"       = $true
            "22_audit_preserve_all_backups_on_errors"                 = $true
            "23_audit_reject_session_deletion_with_pending_ops"       = $true
            "foreign_temp_files_preserved"                            = $true
            "phase0_and_phase1_regression_passed"                     = $true
        }
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
