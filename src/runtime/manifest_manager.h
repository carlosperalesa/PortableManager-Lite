#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pm::runtime {

enum class SessionState {
    Preparing, // PREPARING
    Active,    // ACTIVE
    Cleaning,  // CLEANING
    Cleaned    // CLEANED
};

enum class OperationType {
    KillJob,        // KILL_JOB
    RemoveJunction, // REMOVE_JUNCTION
    RestoreBkp,     // RESTORE_BKP
    RestoreReg,     // RESTORE_REG
    DeleteDir       // DELETE_DIR
};

// Estados unificados de operación en el manifiesto (Sección 5 de Auditoría Fase 2):
// - INTENT_RECORDED: intención persistida en disco antes de modificar el anfitrión.
// - APPLIED: modificación física completada en el anfitrión.
// - ROLLED_BACK: operación inversa ejecutada y verificada satisfactoriamente.
// - FAILED: falló la restauración; los respaldos (.pmbkp / .reg) se conservan intactos.
// - SKIPPED: operación omitida de forma segura porque el cambio físico no llegó a aplicarse.
enum class OperationState {
    IntentRecorded, // INTENT_RECORDED
    Applied,        // APPLIED
    RolledBack,     // ROLLED_BACK
    Failed,         // FAILED
    Skipped         // SKIPPED
};

struct ManifestOperation {
    std::wstring id;
    OperationType type = OperationType::DeleteDir;
    OperationState state = OperationState::IntentRecorded;
    bool original_existed = false;
    std::wstring app_id;
    std::wstring target_host_path; // TargetPath: ruta absoluta en el anfitrión (Junction o directorio)
    std::wstring backup_host_path; // BackupPath: ruta a <TargetPath>.pmbkp o archivo .reg en la sesión (%TEMP%)
    std::wstring reg_key_path;     // RegKeyPath: clave original bajo HKCU\Software\... (para RESTORE_REG)
    std::wstring job_name;         // JobName: nombre del Job Object de la sesión (para KILL_JOB)
    DWORD last_error = ERROR_SUCCESS;
    // Optional phase-4 fields. Legacy REMOVE_JUNCTION entries have no ownership proof.
    std::wstring junction_target;
    std::wstring junction_file_id;
    std::wstring original_file_id;
    std::wstring host_parent_file_id;
    std::wstring registry_backup_sha256;
};

struct ManifestStatusResult {
    bool ok = false;
    bool is_corrupt = false;
    DWORD win32_error = ERROR_SUCCESS;
    std::wstring error_message;
};

struct RollbackSummary {
    bool all_succeeded = false;
    std::size_t total_considered = 0;
    std::size_t rolled_back_count = 0;
    std::size_t skipped_count = 0;
    std::size_t failed_count = 0;
    std::vector<std::wstring> execution_order_ids;
    std::vector<OperationType> execution_order_types;
    std::vector<std::wstring> failed_op_ids;
};

// Adaptadores inyectables para ejecutar las operaciones inversas de subsistemas
// que se implementan en fases posteriores (JobObjects en Fase 3, Junctions en Fase 4, Registro en Fase 5)
// o mediante adaptadores controlados en las pruebas de Fase 2.
struct RollbackHandlers {
    std::function<bool(const ManifestOperation&, DWORD& out_err)> on_kill_job;
    std::function<bool(const ManifestOperation&, DWORD& out_err)> on_remove_junction;
    std::function<bool(const ManifestOperation&, DWORD& out_err)> on_restore_bkp;
    std::function<bool(const ManifestOperation&, DWORD& out_err)> on_restore_reg;
    std::function<bool(const ManifestOperation&, DWORD& out_err)> on_release_reg_backup;
    std::function<bool(const ManifestOperation&, DWORD& out_err)> on_delete_dir;
};

class ManifestManager {
public:
    static inline constexpr wchar_t kFormatSignature[] = L"PM_SESSION_MANIFEST_V1";

    ManifestManager() = default;
    ManifestManager(std::wstring_view session_id, std::wstring_view manifest_file_path);

    void Configure(std::wstring_view session_id, std::wstring_view manifest_file_path);

    const std::wstring& session_id() const noexcept { return session_id_; }
    const std::wstring& manifest_path() const noexcept { return manifest_path_; }
    SessionState session_state() const noexcept { return session_state_; }

    const std::vector<ManifestOperation>& operations() const noexcept { return operations_; }
    const ManifestOperation* FindOperation(std::wstring_view op_id) const noexcept;

    // Comprueba si todas las operaciones registradas han alcanzado un estado terminal aceptable
    // (ROLLED_BACK o SKIPPED).
    bool AreAllOperationsTerminalAcceptable() const noexcept;

    // Inicializa un manifiesto nuevo en estado PREPARING y lo persiste atómicamente.
    // No almacena rutas del USB (PmRootPath) ya que PMG no debe depender del USB para restaurar el anfitrión.
    ManifestStatusResult InitializeNew();

    // Carga y valida un manifiesto existente desde disco, detectando corrupción o manipulación.
    ManifestStatusResult LoadFromDisk();

    // Persiste el estado completo del manifiesto en disco mediante AtomicFile.
    ManifestStatusResult SaveToDisk() const;

    // Actualiza el estado de la sesión y lo persiste inmediatamente en disco.
    // Rechaza pasar a CLEANED si existen operaciones en INTENT_RECORDED, APPLIED o FAILED.
    ManifestStatusResult SetSessionStateAndPersist(SessionState new_state);

    // Regla obligatoria 1 y 2: Registra la intención de una operación (INTENT_RECORDED)
    // y persiste el manifiesto ANTES de modificar el anfitrión.
    // Directiva KILL_JOB: KILL_JOB debe ser la última operación de preparación; no se permite
    // registrar operaciones adicionales de preparación después de KILL_JOB.
    ManifestStatusResult RecordOperationIntent(ManifestOperation op, std::wstring& out_assigned_id);

    // Records the identity of the staging directory before it can be published.
    ManifestStatusResult RecordJunctionFileId(std::wstring_view op_id, std::wstring_view file_id);

    // Regla obligatoria 4 y 5: Actualiza el estado de una operación (ej. APPLIED, ROLLED_BACK, SKIPPED, FAILED)
    // y persiste nuevamente el manifiesto en disco.
    ManifestStatusResult UpdateOperationState(
        std::wstring_view op_id,
        OperationState new_state,
        DWORD last_error = ERROR_SUCCESS);

    // Ejecuta las operaciones inversas pendientes en orden inverso (LIFO) de forma idempotente,
    // garantizando que KILL_JOB se ejecute primero antes de restaurar recursos e inspeccionando
    // el estado físico real de los recursos incluso si la operación quedó en INTENT_RECORDED.
    // Nunca elimina un respaldo (.pmbkp o .reg) si la restauración falla.
    RollbackSummary ExecutePendingRollbacks(const RollbackHandlers& handlers = {});

    // Ejecutores nativos por defecto para carpetas en disco:
    static bool DefaultRestoreBkpDir(
        const ManifestOperation& op,
        bool& out_was_skipped,
        DWORD& out_err) noexcept;

    static bool DefaultDeleteCreatedDir(
        const ManifestOperation& op,
        bool& out_was_skipped,
        DWORD& out_err) noexcept;

    // Valida que un archivo de respaldo .reg exista, no esté truncado ni vacío, contenga cabecera
    // válida de editor de registro y al menos una sección bajo HKEY_CURRENT_USER\Software\.
    static bool ValidateRegBackupFile(const std::wstring& reg_file_path, std::wstring& out_error);

    static bool IsTerminalAcceptableState(OperationState s) noexcept {
        return s == OperationState::RolledBack || s == OperationState::Skipped;
    }

    static std::wstring SessionStateToString(SessionState st);
    static bool ParseSessionState(std::wstring_view text, SessionState& out_st) noexcept;

    static std::wstring OperationTypeToString(OperationType t);
    static bool ParseOperationType(std::wstring_view text, OperationType& out_t) noexcept;

    static std::wstring OperationStateToString(OperationState s);
    static bool ParseOperationState(std::wstring_view text, OperationState& out_s) noexcept;

    // Valida que los campos y rutas de una operación sean seguros y no apunten a ubicaciones prohibidas.
    static bool ValidateOperationSecurity(const ManifestOperation& op, std::wstring& out_error);

private:
    std::wstring session_id_;
    std::wstring manifest_path_;
    SessionState session_state_ = SessionState::Preparing;
    std::vector<ManifestOperation> operations_;
    std::size_t next_op_seq_ = 1;
};

} // namespace pm::runtime
