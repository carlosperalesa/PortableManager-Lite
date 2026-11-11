#pragma once

#include "runtime/manifest_manager.h"

#include <windows.h>
#include <string>
#include <string_view>
#include <vector>

namespace pm::runtime {

struct SessionInitResult {
    bool ok = false;
    DWORD win32_error = ERROR_SUCCESS;
    std::wstring session_id;
    std::wstring session_dir;
    std::wstring manifest_path;
    std::wstring error_message;
};

class SessionManager {
public:
    // Inicializa con la ruta estándar %TEMP%\PortableManager\Session obtenida vía GetTempPathW.
    SessionManager();

    // Constructor para pruebas aisladas dentro de una subcarpeta de %TEMP%.
    explicit SessionManager(std::wstring_view custom_temp_root);

    // Genera un identificador GUID único ("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx") usando CoCreateGuid.
    static std::wstring GenerateSessionGuid();

    // Valida que un SessionID solo contenga caracteres hexadecimales/alfanuméricos y guiones/guiones bajos.
    static bool IsValidSessionId(std::wstring_view session_id) noexcept;

    // Obtiene el directorio base %TEMP% mediante GetTempPathW.
    static std::wstring QuerySystemTempDir();

    // Crea una sesión nueva generando un GUID único, crea %TEMP%\PortableManager\Session\<SessionID>\
    // y persiste session.ini en estado PREPARING.
    // DIRECTIVA SECCIÓN 3: No ejecuta recuperación ni limpieza automática de sesiones antiguas al iniciar PM.
    SessionInitResult InitializeNewSession();
    SessionInitResult InitializeNewSession(std::wstring_view unused_pm_root_dir);

    // Crea una sesión nueva con un SessionID específico válido.
    SessionInitResult InitializeSessionWithId(std::wstring_view session_id);
    SessionInitResult InitializeSessionWithId(
        std::wstring_view session_id,
        std::wstring_view unused_pm_root_dir);

    // Abre la sesión activa indicada por PM desde %TEMP%\PortableManager\Session\<SessionID>\session.ini.
    // Utilizado por PMGuard (--guard <SessionID> <Handle>) para vigilar y restaurar la sesión activa.
    ManifestStatusResult OpenExistingSession(std::wstring_view session_id);

    // Consulta diagnóstica estrictamente pasiva (solo lectura): enumera los SessionIDs presentes en
    // %TEMP%\PortableManager\Session\ cuyo estado no sea CLEANED.
    // DIRECTIVA SECCIÓN 3 (Funcionalidad futura / diagnóstica):
    // - NUNCA se invoca automáticamente al iniciar una nueva instancia de PM.
    // - NUNCA ejecuta limpiezas, rollbacks ni restauraciones automáticas.
    std::vector<std::wstring> DiscoverOrphanSessions() const;

    // Cambia el estado de la sesión (PREPARING -> ACTIVE -> CLEANING -> CLEANED) y lo persiste.
    // Solo permite CLEANED cuando todas las operaciones estén en ROLLED_BACK o SKIPPED.
    ManifestStatusResult TransitionState(SessionState new_state);

    // Consulta el estado actual persistido en session.ini recargando desde disco.
    ManifestStatusResult QueryCurrentStateFromDisk(SessionState& out_state);

    // Finaliza la sesión. Elimina únicamente los archivos temporales propios (*.pmtmp.* y,
    // si remove_manifest_when_cleaned == true y todas las operaciones están en ROLLED_BACK/SKIPPED
    // sin respaldos pendientes, session.ini y la carpeta vacía).
    // Nunca elimina sesiones con operaciones pendientes o fallidas, ni archivos ajenos.
    bool FinalizeSessionAndCleanupOwnFiles(bool remove_manifest_when_cleaned, DWORD& out_error);

    const std::wstring& base_sessions_root() const noexcept { return base_sessions_root_; }
    const std::wstring& session_id() const noexcept { return session_id_; }
    const std::wstring& session_dir() const noexcept { return session_dir_; }
    const std::wstring& manifest_path() const noexcept { return manifest_path_; }
    std::wstring session_mutex_name() const { return L"Local\\PM_Session_Mutex_" + session_id_; }

    ManifestManager& manifest() noexcept { return manifest_; }
    const ManifestManager& manifest() const noexcept { return manifest_; }

private:
    std::wstring base_sessions_root_; // Por defecto: %TEMP%\PortableManager\Session
    std::wstring session_id_;
    std::wstring session_dir_;
    std::wstring manifest_path_;
    ManifestManager manifest_;

    static std::wstring BuildDefaultSessionsRoot(std::wstring_view temp_root);
};

} // namespace pm::runtime
