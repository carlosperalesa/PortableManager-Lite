// ============================================================================
// PortableManager Lite — Ejemplo de Uso de Fase 2 (SessionManager + ManifestManager)
// Archivo: fase02/ejemplo_uso_runtime.cpp
// Descripción:
//   Demuestra el ciclo de vida completo de una sesión en C++20 con:
//   - Comprobación estricta de todos los valores de retorno.
//   - Registro Write-Ahead obligatorio (INTENT_RECORDED -> modificación -> APPLIED).
//   - Registro de KILL_JOB como última operación de preparación antes de ACTIVE.
//   - Ejecución idempotente de rollback en orden inverso (LIFO, con KILL_JOB primero).
//   - Preservación garantizada de respaldos (.pmbkp / .reg) y prohibición de
//     ejecutar remove_all() si existen operaciones fallidas o respaldos pendientes.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#include "../src/core/atomic_file.h"
#include "../src/runtime/manifest_manager.h"
#include "../src/runtime/session_manager.h"

namespace fs = std::filesystem;

int wmain() {
    const std::wstring sysTemp = pm::runtime::SessionManager::QuerySystemTempDir();
    if (sysTemp.empty()) {
        std::wcerr << L"[ERROR] No se pudo obtener %TEMP%.\n";
        return 1;
    }

    const fs::path demoRoot = fs::path(sysTemp) / L"PM_Demo_Fase02";
    const fs::path sessionsRoot = demoRoot / L"Sessions";
    const fs::path hostAppData = demoRoot / L"HostAppData";

    std::error_code ec;
    fs::create_directories(sessionsRoot, ec);
    if (ec) {
        std::wcerr << L"[ERROR] No se pudo crear directorio de sesiones de demostracion.\n";
        return 1;
    }
    fs::create_directories(hostAppData, ec);
    if (ec) {
        std::wcerr << L"[ERROR] No se pudo crear directorio de datos del anfitrion.\n";
        return 1;
    }

    // 1. Inicializar nueva sesión (sin ruta de USB ni recuperación automática de sesiones antiguas)
    pm::runtime::SessionManager sessionMgr(sessionsRoot.wstring());
    const auto initRes = sessionMgr.InitializeNewSession();
    if (!initRes.ok) {
        std::wcerr << L"[ERROR] Fallo InitializeNewSession: " << initRes.error_message << L"\n";
        return 1;
    }
    std::wcout << L"[1] Sesion creada en PREPARING: " << sessionMgr.session_id() << L"\n";

    // 2. Preparar carpeta preexistente simulada en el anfitrión
    const fs::path hostTargetDir = hostAppData / L"DemoApp";
    const fs::path hostBackupDir = hostAppData / L"DemoApp.pmbkp";
    fs::create_directories(hostTargetDir, ec);
    if (ec) {
        std::wcerr << L"[ERROR] No se pudo crear carpeta original del anfitrion.\n";
        return 1;
    }

    const auto writeInit = pm::core::AtomicFile::WriteUtf8Text(
        (hostTargetDir / L"user_config.ini").wstring(),
        "[User]\r\nTheme=Dark\r\n");
    if (!writeInit.ok) {
        std::wcerr << L"[ERROR] No se pudo crear archivo de configuracion original.\n";
        return 1;
    }

    auto& manifest = sessionMgr.manifest();

    // 3. Regla Write-Ahead: registrar INTENT_RECORDED ANTES de renombrar a .pmbkp
    pm::runtime::ManifestOperation opBkp{};
    opBkp.type = pm::runtime::OperationType::RestoreBkp;
    opBkp.original_existed = true;
    opBkp.app_id = L"demo_app";
    opBkp.target_host_path = hostTargetDir.wstring();
    opBkp.backup_host_path = hostBackupDir.wstring();

    std::wstring opBkpId;
    const auto recBkp = manifest.RecordOperationIntent(opBkp, opBkpId);
    if (!recBkp.ok) {
        std::wcerr << L"[ERROR] Fallo RecordOperationIntent (RESTORE_BKP): " << recBkp.error_message << L"\n";
        return 1;
    }

    // 4. Ejecutar el renombrado físico y comprobar explícitamente el resultado de fs::rename
    fs::rename(hostTargetDir, hostBackupDir, ec);
    if (ec || !fs::exists(hostBackupDir)) {
        std::wcerr << L"[ERROR] Fallo fs::rename hacia .pmbkp; se conserva el estado para recuperacion.\n";
        return 1;
    }

    // 5. Confirmar estado APPLIED en el manifiesto y comprobar su resultado
    const auto markBkp = manifest.UpdateOperationState(opBkpId, pm::runtime::OperationState::Applied);
    if (!markBkp.ok) {
        std::wcerr << L"[ERROR] Fallo UpdateOperationState(APPLIED) para RESTORE_BKP: "
                   << markBkp.error_message << L"\n";
        return 1;
    }

    // 6. Directiva KILL_JOB: registrar KILL_JOB como última operación de preparación antes de iniciar la app
    pm::runtime::ManifestOperation opJob{};
    opJob.type = pm::runtime::OperationType::KillJob;
    opJob.app_id = L"demo_app";
    opJob.job_name = L"Local\\PM_Job_" + sessionMgr.session_id();

    std::wstring opJobId;
    const auto recJob = manifest.RecordOperationIntent(opJob, opJobId);
    if (!recJob.ok) {
        std::wcerr << L"[ERROR] Fallo RecordOperationIntent (KILL_JOB): " << recJob.error_message << L"\n";
        return 1;
    }
    const auto markJob = manifest.UpdateOperationState(opJobId, pm::runtime::OperationState::Applied);
    if (!markJob.ok) {
        std::wcerr << L"[ERROR] Fallo UpdateOperationState(APPLIED) para KILL_JOB: "
                   << markJob.error_message << L"\n";
        return 1;
    }

    // 7. Pasar la sesión a estado ACTIVE y comprobar la transición
    const auto toActive = sessionMgr.TransitionState(pm::runtime::SessionState::Active);
    if (!toActive.ok) {
        std::wcerr << L"[ERROR] Fallo TransitionState(ACTIVE): " << toActive.error_message << L"\n";
        return 1;
    }
    std::wcout << L"[2] Sesion en estado ACTIVE.\n";

    // 8. Al finalizar la aplicación, pasar a CLEANING y ejecutar rollbacks en orden LIFO (KILL_JOB primero)
    const auto toCleaning = sessionMgr.TransitionState(pm::runtime::SessionState::Cleaning);
    if (!toCleaning.ok) {
        std::wcerr << L"[ERROR] Fallo TransitionState(CLEANING): " << toCleaning.error_message << L"\n";
        return 1;
    }

    const auto rollbackSummary = manifest.ExecutePendingRollbacks();
    std::wcout << L"[3] Rollback completado: rolled_back=" << rollbackSummary.rolled_back_count
               << L", skipped=" << rollbackSummary.skipped_count
               << L", failed=" << rollbackSummary.failed_count << L"\n";

    // 9. Comprobar que no existan operaciones fallidas ni respaldos pendientes antes de limpiar
    if (!rollbackSummary.all_succeeded || rollbackSummary.failed_count != 0) {
        std::wcerr << L"[WARN] Existen operaciones fallidas (" << rollbackSummary.failed_count
                   << L"). Se preservan intactos el manifiesto y los respaldos (.pmbkp / .reg).\n";
        return 2;
    }

    DWORD finalizeErr = ERROR_SUCCESS;
    if (!sessionMgr.FinalizeSessionAndCleanupOwnFiles(true, finalizeErr)) {
        std::wcerr << L"[WARN] FinalizeSessionAndCleanupOwnFiles rechazo eliminar la sesion (err="
                   << finalizeErr << L"). Se preservan los archivos para restauracion.\n";
        return 3;
    }

    // Solo eliminar el entorno de demostración cuando todas las operaciones han sido restauradas
    // y no queda ningún respaldo .pmbkp pendiente
    if (!fs::exists(hostBackupDir) && fs::exists(hostTargetDir / L"user_config.ini")) {
        fs::remove_all(demoRoot, ec);
        std::wcout << L"[4] Sesion en estado CLEANED, anfitrion restaurado y directorio temporal eliminado.\n";
    } else {
        std::wcerr << L"[WARN] Estado de respaldos inesperado; se conserva demoRoot intacto.\n";
        return 4;
    }

    return 0;
}
