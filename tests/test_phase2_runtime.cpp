#include "core/atomic_file.h"
#include "core/config_manager.h"
#include "core/path_resolver.h"
#include "runtime/manifest_manager.h"
#include "runtime/session_manager.h"

#include <windows.h>
#include <winreg.h>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

using pm::core::AtomicFile;
using pm::runtime::ManifestManager;
using pm::runtime::ManifestOperation;
using pm::runtime::OperationState;
using pm::runtime::OperationType;
using pm::runtime::RollbackHandlers;
using pm::runtime::SessionManager;
using pm::runtime::SessionState;

bool FileOrDirExists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void DeleteTreeForTestCleanup(const std::wstring& dir_path) {
    const std::wstring pattern = dir_path + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring_view name(fd.cFileName);
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring child = dir_path + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                DeleteTreeForTestCleanup(child);
            } else {
                ::SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                ::DeleteFileW(child.c_str());
            }
        } while (::FindNextFileW(hFind, &fd));
        ::FindClose(hFind);
    }
    ::RemoveDirectoryW(dir_path.c_str());
}

bool SpawnChildAndWait(const std::wstring& cmd_line, DWORD& out_exit_code) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd_line;

    if (!::CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    ::WaitForSingleObject(pi.hProcess, 15000);
    ::GetExitCodeProcess(pi.hProcess, &out_exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

std::string MakeValidSyntheticRegContent(std::wstring_view subkey_after_hkcu, std::wstring_view val_name, std::wstring_view val_data) {
    std::wstring wtext = L"Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\";
    wtext.append(subkey_after_hkcu).append(L"]\r\n\"");
    wtext.append(val_name).append(L"\"=\"").append(val_data).append(L"\"\r\n\r\n");
    return pm::core::IniDocument::WideToUtf8(wtext);
}

// Proceso auxiliar para probar reinicio, apertura por PMG y caídas reales abruptas (TerminateProcess)
int RunChildWorkerMode(int argc, wchar_t* argv[]) {
    if (argc < 5) {
        return 90;
    }
    const std::wstring_view mode = argv[2];
    const std::wstring sandbox_temp_root = argv[3];
    const std::wstring session_id = argv[4];

    if (mode == L"verify_reload") {
        SessionManager mgr(sandbox_temp_root);
        auto st = mgr.OpenExistingSession(session_id);
        if (!st.ok) {
            return 91;
        }
        if (mgr.manifest().session_state() != SessionState::Active ||
            mgr.manifest().operations().size() != 2) {
            return 92;
        }
        const auto* op1 = mgr.manifest().FindOperation(L"op_0001");
        if (!op1 || op1->state != OperationState::Applied) {
            return 93;
        }
        return 0;
    }

    if (mode == L"pmg_open_active") {
        SessionManager pmg_mgr(sandbox_temp_root);
        auto st = pmg_mgr.OpenExistingSession(session_id);
        if (!st.ok) {
            return 120;
        }
        if (pmg_mgr.manifest().session_state() != SessionState::Active) {
            return 121;
        }
        // Verificar que el manifiesto no contiene PmRootDir ni PmRootPath y puede restaurar sin USB
        const auto raw_ini = AtomicFile::ReadUtf8Text(pmg_mgr.manifest_path());
        if (!raw_ini.ok ||
            raw_ini.content.find("PmRootDir") != std::string::npos ||
            raw_ini.content.find("PmRootPath") != std::string::npos) {
            return 122;
        }
        auto rb = pmg_mgr.manifest().ExecutePendingRollbacks();
        if (!rb.all_succeeded || pmg_mgr.manifest().session_state() != SessionState::Cleaned) {
            return 123;
        }
        DWORD fin_err = 0;
        if (!pmg_mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err)) {
            return 124;
        }
        return 0;
    }

    if (mode == L"crash_8_before_modify") {
        SessionManager mgr(sandbox_temp_root);
        auto init = mgr.InitializeSessionWithId(session_id);
        if (!init.ok) {
            return 94;
        }
        // Caída real inmediata antes de registrar ni tocar la carpeta del anfitrión
        ::TerminateProcess(::GetCurrentProcess(), 0xDEAD0008U);
        return 0;
    }

    if (mode == L"crash_9_after_intent_before_rename") {
        if (argc < 6) return 95;
        const std::wstring host_dir = argv[5];

        SessionManager mgr(sandbox_temp_root);
        auto init = mgr.InitializeSessionWithId(session_id);
        if (!init.ok) return 95;

        ManifestOperation op{};
        op.type = OperationType::RestoreBkp;
        op.original_existed = true;
        op.app_id = L"synth_app_9";
        op.target_host_path = host_dir;
        op.backup_host_path = host_dir + L".pmbkp";

        std::wstring op_id;
        if (!mgr.manifest().RecordOperationIntent(op, op_id).ok) {
            return 96;
        }
        // Caída real después de persistir INTENT_RECORDED pero ANTES de ejecutar MoveFileExW
        ::TerminateProcess(::GetCurrentProcess(), 0xDEAD0009U);
        return 0;
    }

    if (mode == L"crash_10_after_rename_before_state_update") {
        if (argc < 6) return 97;
        const std::wstring host_dir = argv[5];
        const std::wstring bkp_dir = host_dir + L".pmbkp";

        SessionManager mgr(sandbox_temp_root);
        auto init = mgr.InitializeSessionWithId(session_id);
        if (!init.ok) return 97;

        ManifestOperation op{};
        op.type = OperationType::RestoreBkp;
        op.original_existed = true;
        op.app_id = L"synth_app_10";
        op.target_host_path = host_dir;
        op.backup_host_path = bkp_dir;

        std::wstring op_id;
        if (!mgr.manifest().RecordOperationIntent(op, op_id).ok) {
            return 98;
        }

        // Ejecutar el renombrado físico y crear una carpeta de trabajo sucia en host_dir
        if (!::MoveFileExW(host_dir.c_str(), bkp_dir.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return 99;
        }
        AtomicFile::WriteUtf8Text(host_dir + L"\\dirty_session_file.txt", "DIRTY_SESSION_DATA");

        // Caída real ANTES de actualizar OpState a APPLIED en el manifiesto
        ::TerminateProcess(::GetCurrentProcess(), 0xDEAD000AU);
        return 0;
    }

    if (mode == L"crash_11_after_simulated_junction") {
        if (argc < 7) return 100;
        const std::wstring sim_junction_dir = argv[5];
        const std::wstring sim_portable_dir = argv[6];

        SessionManager mgr(sandbox_temp_root);
        auto init = mgr.InitializeSessionWithId(session_id);
        if (!init.ok) return 101;

        ManifestOperation op{};
        op.type = OperationType::RemoveJunction;
        op.original_existed = false;
        op.app_id = L"synth_app_11";
        op.target_host_path = sim_junction_dir;

        std::wstring op_id;
        if (!mgr.manifest().RecordOperationIntent(op, op_id).ok) {
            return 102;
        }

        // Crear directorio simulado de Junction en el anfitrión y archivo portable en el destino simulado
        AtomicFile::WriteUtf8Text(sim_junction_dir + L"\\junction_marker.sim", "SIMULATED_JUNCTION_LINK");
        AtomicFile::WriteUtf8Text(sim_portable_dir + L"\\portable_user_data.txt", "PORTABLE_DATA_MUST_SURVIVE");

        mgr.manifest().UpdateOperationState(op_id, OperationState::Applied);
        mgr.TransitionState(SessionState::Active);

        // Caída real con la Junction simulada activa
        ::TerminateProcess(::GetCurrentProcess(), 0xDEAD000BU);
        return 0;
    }

    if (mode == L"crash_12_after_simulated_registry") {
        if (argc < 6) return 103;
        const std::wstring sim_reg_store_file = argv[5];

        SessionManager mgr(sandbox_temp_root);
        auto init = mgr.InitializeSessionWithId(session_id);
        if (!init.ok) return 104;

        // Exportar respaldo .reg válido dentro del directorio de sesión ANTES de registrar la intención
        const std::wstring reg_bkp_file = init.session_dir + L"\\sim_app_12_bkp.reg";
        AtomicFile::WriteUtf8Text(
            reg_bkp_file,
            MakeValidSyntheticRegContent(L"Software\\PortableManager\\Tests\\SimApp12", L"Setting", L"HostOriginal_1"));

        ManifestOperation op{};
        op.type = OperationType::RestoreReg;
        op.original_existed = true;
        op.app_id = L"synth_app_12";
        op.reg_key_path = L"HKCU\\Software\\PortableManager\\Tests\\SimApp12";
        op.backup_host_path = reg_bkp_file;

        std::wstring op_id;
        if (!mgr.manifest().RecordOperationIntent(op, op_id).ok) {
            return 105;
        }

        // Simular la modificación de la clave tras persistir INTENT_RECORDED
        AtomicFile::WriteUtf8Text(sim_reg_store_file, "MODIFIED_BY_PORTABLE_APP=999");

        mgr.manifest().UpdateOperationState(op_id, OperationState::Applied);
        mgr.TransitionState(SessionState::Active);

        // Caída real antes de restaurar
        ::TerminateProcess(::GetCurrentProcess(), 0xDEAD000CU);
        return 0;
    }

    return 109;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--worker") {
        return RunChildWorkerMode(argc, argv);
    }

    const std::wstring self_exe = pm::core::PathResolver::QueryExecutablePath();
    if (self_exe.empty()) {
        std::fwprintf(stderr, L"[FAIL] No se pudo obtener la ruta de test_phase2_runtime.exe\n");
        return 1;
    }

    const std::wstring sys_temp = SessionManager::QuerySystemTempDir();
    const std::wstring sandbox_root =
        sys_temp + L"\\PM_Phase2_RuntimeTest_" + std::to_wstring(::GetCurrentProcessId());
    DeleteTreeForTestCleanup(sandbox_root);
    ::CreateDirectoryW(sandbox_root.c_str(), nullptr);

    // Crear un archivo ajeno en %TEMP% dentro del sandbox para comprobar que nunca es eliminado por SessionManager
    const std::wstring foreign_temp_file = sandbox_root + L"\\foreign_unrelated_file.tmp";
    AtomicFile::WriteUtf8Text(foreign_temp_file, "UNRELATED_TEMP_FILE_MUST_SURVIVE");

    auto fail_and_cleanup = [&](const wchar_t* msg, int code) -> int {
        std::fwprintf(stderr, L"[FAIL] %ls (code=%d)\n", msg, code);
        DeleteTreeForTestCleanup(sandbox_root);
        return code;
    };

    // =========================================================================
    // ESCENARIOS 1, 2, 3, 4, 5, 6 y 7:
    // Creación de sesión, generación de GUID, creación del manifiesto,
    // registro de operación, persistencia previa, actualización de estado
    // y lectura después de reinicio en otro proceso.
    // =========================================================================
    {
        const std::wstring guid1 = SessionManager::GenerateSessionGuid();
        const std::wstring guid2 = SessionManager::GenerateSessionGuid();
        if (guid1.size() != 36 || guid2.size() != 36 || guid1 == guid2 ||
            !SessionManager::IsValidSessionId(guid1)) {
            return fail_and_cleanup(L"Escenario 2: Generacion de GUID invalida.", 10);
        }

        SessionManager mgr(sandbox_root);
        auto init = mgr.InitializeNewSession();
        if (!init.ok || !FileOrDirExists(init.manifest_path) ||
            mgr.manifest().session_state() != SessionState::Preparing) {
            return fail_and_cleanup(L"Escenarios 1 y 3: Fallo inicializacion de sesion y manifiesto.", 11);
        }

        const std::wstring dir_op1 = sandbox_root + L"\\HostScenario1_Dir";
        ManifestOperation op1{};
        op1.type = OperationType::DeleteDir;
        op1.original_existed = false;
        op1.app_id = L"app_one";
        op1.target_host_path = dir_op1;

        std::wstring op1_id;
        if (!mgr.manifest().RecordOperationIntent(op1, op1_id).ok || op1_id != L"op_0001") {
            return fail_and_cleanup(L"Escenario 4: Fallo al registrar operacion op_0001.", 12);
        }

        // Escenario 5: Verificar en disco que la intención quedó persistida ANTES de crear dir_op1
        {
            ManifestManager disk_check(init.session_id, init.manifest_path);
            if (!disk_check.LoadFromDisk().ok ||
                disk_check.FindOperation(L"op_0001") == nullptr ||
                disk_check.FindOperation(L"op_0001")->state != OperationState::IntentRecorded ||
                FileOrDirExists(dir_op1)) {
                return fail_and_cleanup(L"Escenario 5: La intencion no se persistio antes de modificar el anfitrion.", 13);
            }
        }

        // Ahora realizar la modificación y actualizar estado a APPLIED (Escenario 6)
        ::CreateDirectoryW(dir_op1.c_str(), nullptr);
        if (!mgr.manifest().UpdateOperationState(op1_id, OperationState::Applied).ok) {
            return fail_and_cleanup(L"Escenario 6: Fallo al actualizar estado de op_0001 a APPLIED.", 14);
        }

        // Registrar KILL_JOB como última operación de preparación inmediatamente antes de iniciar la app
        ManifestOperation op2{};
        op2.type = OperationType::KillJob;
        op2.app_id = L"app_one";
        op2.job_name = L"Local\\PM_Job_" + init.session_id;
        std::wstring op2_id;
        if (!mgr.manifest().RecordOperationIntent(op2, op2_id).ok ||
            !mgr.manifest().UpdateOperationState(op2_id, OperationState::Applied).ok ||
            !mgr.TransitionState(SessionState::Active).ok) {
            return fail_and_cleanup(L"Escenario 6: Fallo al registrar KILL_JOB y pasar a ACTIVE.", 15);
        }

        // Escenario 7: Lectura después de reinicio en un proceso hijo independiente
        DWORD child_rc = 999;
        const std::wstring cmd = L"\"" + self_exe + L"\" --worker verify_reload \"" +
                                 sandbox_root + L"\" \"" + init.session_id + L"\"";
        if (!SpawnChildAndWait(cmd, child_rc) || child_rc != 0) {
            return fail_and_cleanup(L"Escenario 7: Proceso hijo no pudo reconstruir el manifiesto.", 16);
        }

        // Limpiar y finalizar la sesión 1
        auto rb = mgr.manifest().ExecutePendingRollbacks();
        DWORD fin_err = 0;
        if (!rb.all_succeeded || !mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err) ||
            FileOrDirExists(dir_op1) || FileOrDirExists(init.manifest_path)) {
            return fail_and_cleanup(L"Escenario 7: Finalizacion de sesion 1 fallo.", 17);
        }
    }

    // =========================================================================
    // ESCENARIO 8: Caída real del proceso antes de modificar
    // =========================================================================
    {
        const std::wstring sid8 = L"session_crash_08";
        const std::wstring untouched_dir = sandbox_root + L"\\Scenario8_HostDir";
        AtomicFile::WriteUtf8Text(untouched_dir + L"\\original.txt", "HOST_ORIGINAL_8");

        DWORD crash_rc = 0;
        const std::wstring cmd = L"\"" + self_exe + L"\" --worker crash_8_before_modify \"" +
                                 sandbox_root + L"\" \"" + sid8 + L"\"";
        if (!SpawnChildAndWait(cmd, crash_rc) || crash_rc != 0xDEAD0008U) {
            return fail_and_cleanup(L"Escenario 8: El proceso auxiliar no experimento la caida esperada.", 20);
        }

        SessionManager recovery_mgr(sandbox_root);
        if (!recovery_mgr.OpenExistingSession(sid8).ok) {
            return fail_and_cleanup(L"Escenario 8: No se pudo abrir la sesion tras caida previa a modificar.", 21);
        }
        auto rb = recovery_mgr.manifest().ExecutePendingRollbacks();
        auto rd = AtomicFile::ReadUtf8Text(untouched_dir + L"\\original.txt");
        if (!rb.all_succeeded || !rd.ok || rd.content != "HOST_ORIGINAL_8") {
            return fail_and_cleanup(L"Escenario 8: La carpeta intacta del anfitrion se vio afectada.", 22);
        }
        DWORD fin_err = 0;
        recovery_mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err);
    }

    // =========================================================================
    // ESCENARIO 9: Caída real después de registrar intención (antes de renombrar -> SKIPPED)
    // =========================================================================
    {
        const std::wstring sid9 = L"session_crash_09";
        const std::wstring host_dir9 = sandbox_root + L"\\Scenario9_HostDir";
        AtomicFile::WriteUtf8Text(host_dir9 + L"\\profile.txt", "HOST_PROFILE_9_INTACT");

        DWORD crash_rc = 0;
        const std::wstring cmd = L"\"" + self_exe + L"\" --worker crash_9_after_intent_before_rename \"" +
                                 sandbox_root + L"\" \"" + sid9 + L"\" \"" + host_dir9 + L"\"";
        if (!SpawnChildAndWait(cmd, crash_rc) || crash_rc != 0xDEAD0009U) {
            return fail_and_cleanup(L"Escenario 9: El proceso auxiliar no cayo con 0xDEAD0009.", 30);
        }

        SessionManager recovery_mgr(sandbox_root);
        if (!recovery_mgr.OpenExistingSession(sid9).ok) {
            return fail_and_cleanup(L"Escenario 9: No se pudo abrir sesion tras caida en INTENT_RECORDED.", 31);
        }
        auto rb = recovery_mgr.manifest().ExecutePendingRollbacks();
        const auto* op9 = recovery_mgr.manifest().FindOperation(L"op_0001");
        auto rd = AtomicFile::ReadUtf8Text(host_dir9 + L"\\profile.txt");
        if (!rb.all_succeeded || rb.skipped_count != 1 ||
            !op9 || op9->state != OperationState::Skipped ||
            !rd.ok || rd.content != "HOST_PROFILE_9_INTACT" ||
            FileOrDirExists(host_dir9 + L".pmbkp")) {
            return fail_and_cleanup(L"Escenario 9: Fallo la recuperacion SKIPPED cuando la caida ocurrio antes de renombrar.", 32);
        }
        DWORD fin_err = 0;
        recovery_mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err);
    }

    // =========================================================================
    // ESCENARIO 10: Caída real después de renombrar a .pmbkp (antes de marcar APPLIED -> ROLLED_BACK)
    // =========================================================================
    {
        const std::wstring sid10 = L"session_crash_10";
        const std::wstring host_dir10 = sandbox_root + L"\\Scenario10_HostDir";
        AtomicFile::WriteUtf8Text(host_dir10 + L"\\user_data.txt", "HOST_ORIGINAL_DATA_10");

        DWORD crash_rc = 0;
        const std::wstring cmd = L"\"" + self_exe + L"\" --worker crash_10_after_rename_before_state_update \"" +
                                 sandbox_root + L"\" \"" + sid10 + L"\" \"" + host_dir10 + L"\"";
        if (!SpawnChildAndWait(cmd, crash_rc) || crash_rc != 0xDEAD000AU) {
            return fail_and_cleanup(L"Escenario 10: El proceso auxiliar no cayo con 0xDEAD000A.", 40);
        }

        SessionManager recovery_mgr(sandbox_root);
        if (!recovery_mgr.OpenExistingSession(sid10).ok) {
            return fail_and_cleanup(L"Escenario 10: No se pudo cargar el manifiesto tras la caida.", 41);
        }
        const auto* op = recovery_mgr.manifest().FindOperation(L"op_0001");
        if (!op || op->state != OperationState::IntentRecorded ||
            !FileOrDirExists(host_dir10 + L".pmbkp")) {
            return fail_and_cleanup(L"Escenario 10: Estado previo a la restauracion inesperado.", 42);
        }

        auto rb = recovery_mgr.manifest().ExecutePendingRollbacks();
        const auto* op_after = recovery_mgr.manifest().FindOperation(L"op_0001");
        auto rd = AtomicFile::ReadUtf8Text(host_dir10 + L"\\user_data.txt");
        if (!rb.all_succeeded || rb.rolled_back_count != 1 ||
            !op_after || op_after->state != OperationState::RolledBack ||
            !rd.ok || rd.content != "HOST_ORIGINAL_DATA_10" ||
            FileOrDirExists(host_dir10 + L".pmbkp") ||
            FileOrDirExists(host_dir10 + L"\\dirty_session_file.txt")) {
            return fail_and_cleanup(L"Escenario 10: No se restauro .pmbkp cuando la caida ocurrio antes de APPLIED.", 43);
        }
        DWORD fin_err = 0;
        recovery_mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err);
    }

    // =========================================================================
    // ESCENARIO 11: Caída real después de registrar una Junction simulada
    // =========================================================================
    {
        const std::wstring sid11 = L"session_crash_11";
        const std::wstring sim_junction = sandbox_root + L"\\Scenario11_SimJunction";
        const std::wstring sim_portable = sandbox_root + L"\\Scenario11_PortableTarget";

        DWORD crash_rc = 0;
        const std::wstring cmd = L"\"" + self_exe + L"\" --worker crash_11_after_simulated_junction \"" +
                                 sandbox_root + L"\" \"" + sid11 + L"\" \"" +
                                 sim_junction + L"\" \"" + sim_portable + L"\"";
        if (!SpawnChildAndWait(cmd, crash_rc) || crash_rc != 0xDEAD000BU) {
            return fail_and_cleanup(L"Escenario 11: El proceso auxiliar no cayo con 0xDEAD000B.", 50);
        }

        SessionManager recovery_mgr(sandbox_root);
        if (!recovery_mgr.OpenExistingSession(sid11).ok) {
            return fail_and_cleanup(L"Escenario 11: No se pudo abrir el manifiesto de Junction simulada.", 51);
        }

        bool junction_adapter_called = false;
        RollbackHandlers handlers{};
        handlers.on_remove_junction = [&](const ManifestOperation& mop, DWORD& out_err) -> bool {
            junction_adapter_called = true;
            bool skipped = false;
            return ManifestManager::DefaultDeleteCreatedDir(mop, skipped, out_err);
        };

        auto rb = recovery_mgr.manifest().ExecutePendingRollbacks(handlers);
        auto portable_rd = AtomicFile::ReadUtf8Text(sim_portable + L"\\portable_user_data.txt");
        if (!rb.all_succeeded || !junction_adapter_called ||
            FileOrDirExists(sim_junction) ||
            !portable_rd.ok || portable_rd.content != "PORTABLE_DATA_MUST_SURVIVE") {
            return fail_and_cleanup(L"Escenario 11: Fallo rollback de Junction simulada o se alteraron datos portables.", 52);
        }
        DWORD fin_err = 0;
        recovery_mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err);
    }

    // =========================================================================
    // ESCENARIO 12: Caída real después de registrar una modificación de registro con respaldo .reg
    // =========================================================================
    {
        const std::wstring sid12 = L"session_crash_12";
        const std::wstring sim_reg_file = sandbox_root + L"\\Scenario12_SimRegStore.dat";

        DWORD crash_rc = 0;
        const std::wstring cmd = L"\"" + self_exe + L"\" --worker crash_12_after_simulated_registry \"" +
                                 sandbox_root + L"\" \"" + sid12 + L"\" \"" + sim_reg_file + L"\"";
        if (!SpawnChildAndWait(cmd, crash_rc) || crash_rc != 0xDEAD000CU) {
            return fail_and_cleanup(L"Escenario 12: El proceso auxiliar no cayo con 0xDEAD000C.", 60);
        }

        SessionManager recovery_mgr(sandbox_root);
        if (!recovery_mgr.OpenExistingSession(sid12).ok) {
            return fail_and_cleanup(L"Escenario 12: No se pudo abrir el manifiesto de registro simulado.", 61);
        }

        bool reg_adapter_called = false;
        std::wstring reg_bkp_path_observed;
        RollbackHandlers handlers{};
        handlers.on_restore_reg = [&](const ManifestOperation& mop, DWORD& out_err) -> bool {
            reg_adapter_called = true;
            reg_bkp_path_observed = mop.backup_host_path;
            if (mop.reg_key_path != L"HKCU\\Software\\PortableManager\\Tests\\SimApp12") {
                out_err = ERROR_INVALID_PARAMETER;
                return false;
            }
            const auto bkp_rd = AtomicFile::ReadUtf8Text(mop.backup_host_path);
            if (!bkp_rd.ok) {
                out_err = ERROR_FILE_NOT_FOUND;
                return false;
            }
            AtomicFile::WriteUtf8Text(sim_reg_file, "ORIG_REG_VALUE=HostOriginal_1");
            out_err = ERROR_SUCCESS;
            return true;
        };

        auto rb = recovery_mgr.manifest().ExecutePendingRollbacks(handlers);
        auto reg_rd = AtomicFile::ReadUtf8Text(sim_reg_file);
        if (!rb.all_succeeded || !reg_adapter_called ||
            !reg_rd.ok || reg_rd.content != "ORIG_REG_VALUE=HostOriginal_1" ||
            FileOrDirExists(reg_bkp_path_observed)) {
            return fail_and_cleanup(L"Escenario 12: Fallo rollback de registro con archivo .reg.", 62);
        }
        DWORD fin_err = 0;
        recovery_mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err);
    }

    // =========================================================================
    // ESCENARIOS 13, 14 y 16:
    // Restauración parcial, conservación intacta de respaldos ante error
    // y repetición idempotente de operaciones inversas.
    // =========================================================================
    {
        SessionManager mgr(sandbox_root);
        auto init = mgr.InitializeSessionWithId(L"session_partial_13");
        if (!init.ok) {
            return fail_and_cleanup(L"Escenario 13: No se pudo iniciar sesion.", 70);
        }

        // Rechazo de sobrescritura de un respaldo .pmbkp ya existente (Escenario 16a)
        const std::wstring pre_existing_bkp_target = sandbox_root + L"\\PreExistingBkpDir";
        AtomicFile::WriteUtf8Text(pre_existing_bkp_target + L".pmbkp\\important.txt", "DO_NOT_OVERWRITE_ME");
        ManifestOperation bad_bkp_op{};
        bad_bkp_op.type = OperationType::RestoreBkp;
        bad_bkp_op.original_existed = true;
        bad_bkp_op.target_host_path = pre_existing_bkp_target;
        bad_bkp_op.backup_host_path = pre_existing_bkp_target + L".pmbkp";
        std::wstring unused_id;
        if (mgr.manifest().RecordOperationIntent(bad_bkp_op, unused_id).ok) {
            return fail_and_cleanup(L"Escenario 16a: Se permitio registrar una operacion que sobrescribiria un .pmbkp previo.", 71);
        }

        const std::wstring dir_a = sandbox_root + L"\\Partial_DirA";
        const std::wstring dir_b = sandbox_root + L"\\Partial_DirB";
        const std::wstring dir_b_bkp = dir_b + L".pmbkp";
        const std::wstring dir_c = sandbox_root + L"\\Partial_DirC";

        AtomicFile::WriteUtf8Text(dir_b + L"\\original_b.txt", "ORIGINAL_B_MUST_BE_PRESERVED");

        ManifestOperation op_a{};
        op_a.type = OperationType::DeleteDir;
        op_a.original_existed = false;
        op_a.target_host_path = dir_a;

        ManifestOperation op_b{};
        op_b.type = OperationType::RestoreBkp;
        op_b.original_existed = true;
        op_b.target_host_path = dir_b;
        op_b.backup_host_path = dir_b_bkp;

        ManifestOperation op_c{};
        op_c.type = OperationType::DeleteDir;
        op_c.original_existed = false;
        op_c.target_host_path = dir_c;

        std::wstring id_a, id_b, id_c;
        if (!mgr.manifest().RecordOperationIntent(op_a, id_a).ok ||
            !mgr.manifest().RecordOperationIntent(op_b, id_b).ok ||
            !mgr.manifest().RecordOperationIntent(op_c, id_c).ok) {
            return fail_and_cleanup(L"Escenario 13: Fallo registro de las 3 operaciones.", 72);
        }

        ::CreateDirectoryW(dir_a.c_str(), nullptr);
        ::MoveFileExW(dir_b.c_str(), dir_b_bkp.c_str(), MOVEFILE_WRITE_THROUGH);
        ::CreateDirectoryW(dir_b.c_str(), nullptr);
        ::CreateDirectoryW(dir_c.c_str(), nullptr);

        mgr.manifest().UpdateOperationState(id_a, OperationState::Applied);
        mgr.manifest().UpdateOperationState(id_b, OperationState::Applied);
        mgr.manifest().UpdateOperationState(id_c, OperationState::Applied);

        // Bloquear un archivo dentro de la carpeta sucia dir_b para provocar fallo real en op_b
        const std::wstring locked_file = dir_b + L"\\locked.dat";
        HANDLE hLock = ::CreateFileW(
            locked_file.c_str(),
            GENERIC_WRITE,
            0, // Sin FILE_SHARE_DELETE -> impide borrar dir_b
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (hLock == INVALID_HANDLE_VALUE) {
            return fail_and_cleanup(L"Escenario 13: No se pudo crear archivo de bloqueo.", 73);
        }

        // Primer intento: debe restaurar op_c y op_a, pero fallar en op_b conservando dir_b_bkp intacto
        auto rb_partial = mgr.manifest().ExecutePendingRollbacks();
        auto bkp_check = AtomicFile::ReadUtf8Text(dir_b_bkp + L"\\original_b.txt");
        DWORD fin_err_while_failed = 0;
        const bool fin_rejected = !mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err_while_failed);

        if (rb_partial.all_succeeded || rb_partial.rolled_back_count != 2 || rb_partial.failed_count != 1 ||
            mgr.manifest().session_state() == SessionState::Cleaned ||
            !fin_rejected ||
            FileOrDirExists(dir_a) || FileOrDirExists(dir_c) ||
            !bkp_check.ok || bkp_check.content != "ORIGINAL_B_MUST_BE_PRESERVED") {
            ::CloseHandle(hLock);
            return fail_and_cleanup(L"Escenario 13/16b: Fallo restauracion parcial o no se conservo el respaldo .pmbkp.", 74);
        }

        // Liberar el bloqueo y ejecutar nuevamente ExecutePendingRollbacks (Recuperación + Idempotencia Escenario 14)
        ::CloseHandle(hLock);
        auto rb_retry = mgr.manifest().ExecutePendingRollbacks();
        auto restored_b = AtomicFile::ReadUtf8Text(dir_b + L"\\original_b.txt");
        if (!rb_retry.all_succeeded || rb_retry.failed_count != 0 ||
            mgr.manifest().session_state() != SessionState::Cleaned ||
            !restored_b.ok || restored_b.content != "ORIGINAL_B_MUST_BE_PRESERVED" ||
            FileOrDirExists(dir_b_bkp)) {
            return fail_and_cleanup(L"Escenario 13/14: El segundo intento tras desbloqueo no completo la restauracion.", 75);
        }

        // Escenario 14: Ejecutar una tercera vez ExecutePendingRollbacks sobre el manifiesto ya restaurado
        auto rb_idempotent = mgr.manifest().ExecutePendingRollbacks();
        auto restored_b_again = AtomicFile::ReadUtf8Text(dir_b + L"\\original_b.txt");
        if (!rb_idempotent.all_succeeded || !restored_b_again.ok ||
            restored_b_again.content != "ORIGINAL_B_MUST_BE_PRESERVED") {
            return fail_and_cleanup(L"Escenario 14: La repeticion idempotente de operaciones inversas fallo.", 76);
        }
        DWORD fin_err = 0;
        mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err);
    }

    // =========================================================================
    // ESCENARIO 15: Detección de manifiestos corruptos, incompletos o manipulados
    // =========================================================================
    {
        const std::wstring corrupt_dir = sandbox_root + L"\\CorruptTests";
        ::CreateDirectoryW(corrupt_dir.c_str(), nullptr);

        // 15.1 Manifiesto truncado (sin [Footer] EndMarker=EOF_OK)
        const std::wstring truncated_path = corrupt_dir + L"\\truncated.ini";
        const std::string truncated_content =
            "[Session]\r\n"
            "FormatSignature=PM_SESSION_MANIFEST_V1\r\n"
            "SessionId=corrupt_1\r\n"
            "State=ACTIVE\r\n"
            "OperationCount=0\r\n";
        AtomicFile::WriteUtf8Text(truncated_path, truncated_content);
        ManifestManager mm_trunc(L"corrupt_1", truncated_path);
        auto r_trunc = mm_trunc.LoadFromDisk();
        if (r_trunc.ok || !r_trunc.is_corrupt) {
            return fail_and_cleanup(L"Escenario 15.1: No se detecto manifiesto truncado.", 80);
        }

        // 15.2 Manifiesto con tipo de operación desconocido / arbitrario
        const std::wstring unknown_op_path = corrupt_dir + L"\\unknown_op.ini";
        const std::string unknown_op_content =
            "[Session]\r\n"
            "FormatSignature=PM_SESSION_MANIFEST_V1\r\n"
            "SessionId=corrupt_2\r\n"
            "State=ACTIVE\r\n"
            "OperationCount=1\r\n"
            "\r\n[Op.op_0001]\r\n"
            "OpId=op_0001\r\n"
            "OpType=RUN_ARBITRARY_CMD\r\n"
            "OpState=APPLIED\r\n"
            "OriginalExisted=0\r\n"
            "TargetPath=C:\\Windows\\System32\\cmd.exe\r\n"
            "\r\n[Footer]\r\n"
            "EndMarker=EOF_OK\r\n";
        AtomicFile::WriteUtf8Text(unknown_op_path, unknown_op_content);
        ManifestManager mm_unk(L"corrupt_2", unknown_op_path);
        auto r_unk = mm_unk.LoadFromDisk();
        if (r_unk.ok || !r_unk.is_corrupt) {
            return fail_and_cleanup(L"Escenario 15.2: No se rechazo tipo de operacion arbitrario.", 81);
        }

        // 15.3 Manifiesto manipulado ordenando eliminar una ruta fuera del perfil de usuario (C:\Windows\System32)
        const std::wstring forged_path = corrupt_dir + L"\\forged_target.ini";
        const std::string forged_content =
            "[Session]\r\n"
            "FormatSignature=PM_SESSION_MANIFEST_V1\r\n"
            "SessionId=corrupt_3\r\n"
            "State=ACTIVE\r\n"
            "OperationCount=1\r\n"
            "\r\n[Op.op_0001]\r\n"
            "OpId=op_0001\r\n"
            "OpType=DELETE_DIR\r\n"
            "OpState=APPLIED\r\n"
            "OriginalExisted=0\r\n"
            "TargetPath=C:\\Windows\\System32\\drivers\r\n"
            "\r\n[Footer]\r\n"
            "EndMarker=EOF_OK\r\n";
        AtomicFile::WriteUtf8Text(forged_path, forged_content);
        ManifestManager mm_forged(L"corrupt_3", forged_path);
        auto r_forged = mm_forged.LoadFromDisk();
        if (r_forged.ok || !r_forged.is_corrupt) {
            return fail_and_cleanup(L"Escenario 15.3: No se rechazo ruta de sistema manipulada en manifiesto.", 82);
        }
    }

    // =========================================================================
    // ESCENARIO 17 (AUDITORÍA): Orden efectivo de limpieza — KILL_JOB primero
    // =========================================================================
    {
        SessionManager mgr(sandbox_root);
        auto init = mgr.InitializeSessionWithId(L"session_audit_17_killjob_order");
        if (!init.ok) {
            return fail_and_cleanup(L"Escenario 17: No se pudo crear sesion.", 170);
        }

        const std::wstring app_dir = sandbox_root + L"\\Audit17_AppDir";
        const std::wstring app_bkp = app_dir + L".pmbkp";
        const std::wstring temp_work = sandbox_root + L"\\Audit17_WorkDir";
        const std::wstring reg_bkp = init.session_dir + L"\\audit17_reg.reg";

        AtomicFile::WriteUtf8Text(app_dir + L"\\data.txt", "ORIG_DATA_17");
        AtomicFile::WriteUtf8Text(
            reg_bkp,
            MakeValidSyntheticRegContent(L"Software\\PortableManager\\Tests\\Audit17", L"Key1", L"Val1"));

        // 1. Registrar RESTORE_BKP
        ManifestOperation op1{};
        op1.type = OperationType::RestoreBkp;
        op1.original_existed = true;
        op1.target_host_path = app_dir;
        op1.backup_host_path = app_bkp;

        // 2. Registrar REMOVE_JUNCTION
        ManifestOperation op2{};
        op2.type = OperationType::RemoveJunction;
        op2.original_existed = true;
        op2.target_host_path = app_dir;

        // 3. Registrar RESTORE_REG
        ManifestOperation op3{};
        op3.type = OperationType::RestoreReg;
        op3.original_existed = true;
        op3.reg_key_path = L"HKCU\\Software\\PortableManager\\Tests\\Audit17";
        op3.backup_host_path = reg_bkp;

        // 4. Registrar DELETE_DIR
        ManifestOperation op4{};
        op4.type = OperationType::DeleteDir;
        op4.original_existed = false;
        op4.target_host_path = temp_work;

        // 5. Registrar KILL_JOB como última operación de preparación inmediatamente antes de iniciar la app
        ManifestOperation op5{};
        op5.type = OperationType::KillJob;
        op5.job_name = L"Local\\PM_Job_Audit17";

        std::wstring id1, id2, id3, id4, id5;
        if (!mgr.manifest().RecordOperationIntent(op1, id1).ok ||
            !mgr.manifest().RecordOperationIntent(op2, id2).ok ||
            !mgr.manifest().RecordOperationIntent(op3, id3).ok ||
            !mgr.manifest().RecordOperationIntent(op4, id4).ok ||
            !mgr.manifest().RecordOperationIntent(op5, id5).ok) {
            return fail_and_cleanup(L"Escenario 17: Fallo al registrar las 5 operaciones en orden de preparacion.", 171);
        }

        // Comprobar que intentar registrar otra operación de recurso DESPUÉS de KILL_JOB es rechazado
        ManifestOperation op_after_kill{};
        op_after_kill.type = OperationType::DeleteDir;
        op_after_kill.original_existed = false;
        op_after_kill.target_host_path = sandbox_root + L"\\AfterKillDir";
        std::wstring id_rejected;
        if (mgr.manifest().RecordOperationIntent(op_after_kill, id_rejected).ok) {
            return fail_and_cleanup(L"Escenario 17: Se permitio registrar una operacion de preparacion despues de KILL_JOB.", 172);
        }

        bool process_terminated_flag = false;
        bool resource_restored_before_kill = false;
        std::vector<OperationType> callback_order;

        RollbackHandlers handlers{};
        handlers.on_kill_job = [&](const ManifestOperation&, DWORD& out_err) -> bool {
            callback_order.push_back(OperationType::KillJob);
            process_terminated_flag = true;
            out_err = ERROR_SUCCESS;
            return true;
        };
        handlers.on_delete_dir = [&](const ManifestOperation&, DWORD& out_err) -> bool {
            if (!process_terminated_flag) resource_restored_before_kill = true;
            callback_order.push_back(OperationType::DeleteDir);
            out_err = ERROR_SUCCESS;
            return true;
        };
        handlers.on_restore_reg = [&](const ManifestOperation&, DWORD& out_err) -> bool {
            if (!process_terminated_flag) resource_restored_before_kill = true;
            callback_order.push_back(OperationType::RestoreReg);
            out_err = ERROR_SUCCESS;
            return true;
        };
        handlers.on_remove_junction = [&](const ManifestOperation&, DWORD& out_err) -> bool {
            if (!process_terminated_flag) resource_restored_before_kill = true;
            callback_order.push_back(OperationType::RemoveJunction);
            out_err = ERROR_SUCCESS;
            return true;
        };
        handlers.on_restore_bkp = [&](const ManifestOperation&, DWORD& out_err) -> bool {
            if (!process_terminated_flag) resource_restored_before_kill = true;
            callback_order.push_back(OperationType::RestoreBkp);
            out_err = ERROR_SUCCESS;
            return true;
        };

        auto rb = mgr.manifest().ExecutePendingRollbacks(handlers);
        if (!rb.all_succeeded ||
            resource_restored_before_kill ||
            callback_order.size() != 5 ||
            callback_order[0] != OperationType::KillJob ||
            callback_order[1] != OperationType::DeleteDir ||
            callback_order[2] != OperationType::RestoreReg ||
            callback_order[3] != OperationType::RemoveJunction ||
            callback_order[4] != OperationType::RestoreBkp) {
            return fail_and_cleanup(L"Escenario 17: El orden efectivo de ejecucion no respeto KILL_JOB primero en LIFO.", 173);
        }
        DWORD fin_err = 0;
        mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err);
    }

    // =========================================================================
    // ESCENARIO 18 (AUDITORÍA): Respaldo del registro mediante archivos .reg sobre clave sintética HKCU
    // =========================================================================
    {
        SessionManager mgr(sandbox_root);
        auto init = mgr.InitializeSessionWithId(L"session_audit_18_reg_file");
        if (!init.ok) {
            return fail_and_cleanup(L"Escenario 18: No se pudo crear sesion.", 180);
        }

        // Clave sintética exclusiva en HKCU
        const std::wstring synth_subkey =
            L"Software\\PortableManager\\Tests\\Phase2Audit_" + std::to_wstring(::GetCurrentProcessId());
        const std::wstring full_hkcu_key = L"HKCU\\" + synth_subkey;
        ::RegDeleteTreeW(HKEY_CURRENT_USER, synth_subkey.c_str());

        // 1. Crear clave sintética original en HKCU
        HKEY hKey = nullptr;
        if (::RegCreateKeyExW(HKEY_CURRENT_USER, synth_subkey.c_str(), 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
            return fail_and_cleanup(L"Escenario 18: No se pudo crear clave sintetica en HKCU.", 181);
        }
        const wchar_t orig_val[] = L"HostOriginalValue_18";
        ::RegSetValueExW(hKey, L"Theme", 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(orig_val),
                         static_cast<DWORD>(sizeof(orig_val)));
        ::RegCloseKey(hKey);

        // 2. Comprobar que una exportación .reg truncada/incompleta es rechazada
        const std::wstring truncated_reg_path = init.session_dir + L"\\incomplete_export.reg";
        AtomicFile::WriteUtf8Text(truncated_reg_path, "Windows Registry Editor Version 5.00\r\n[HKEY_CURRENT_USER\\Software\\Truncated");
        std::wstring val_err;
        if (ManifestManager::ValidateRegBackupFile(truncated_reg_path, val_err)) {
            ::RegDeleteTreeW(HKEY_CURRENT_USER, synth_subkey.c_str());
            return fail_and_cleanup(L"Escenario 18: ValidateRegBackupFile acepto un archivo .reg truncado.", 182);
        }

        ManifestOperation bad_reg_op{};
        bad_reg_op.type = OperationType::RestoreReg;
        bad_reg_op.original_existed = true;
        bad_reg_op.reg_key_path = full_hkcu_key;
        bad_reg_op.backup_host_path = truncated_reg_path;
        std::wstring rejected_id;
        if (mgr.manifest().RecordOperationIntent(bad_reg_op, rejected_id).ok) {
            ::RegDeleteTreeW(HKEY_CURRENT_USER, synth_subkey.c_str());
            return fail_and_cleanup(L"Escenario 18: RecordOperationIntent acepto un respaldo .reg incompleto.", 183);
        }
        ::DeleteFileW(truncated_reg_path.c_str());

        // 3. Exportar la clave sintética a un archivo .reg dentro del directorio de sesión usando reg.exe export
        const std::wstring valid_reg_path = init.session_dir + L"\\hkcu_synth_backup.reg";
        DWORD reg_export_rc = 999;
        const std::wstring export_cmd = L"reg.exe export \"" + full_hkcu_key + L"\" \"" + valid_reg_path + L"\" /y";
        if (!SpawnChildAndWait(export_cmd, reg_export_rc) || reg_export_rc != 0 ||
            !ManifestManager::ValidateRegBackupFile(valid_reg_path, val_err)) {
            ::RegDeleteTreeW(HKEY_CURRENT_USER, synth_subkey.c_str());
            return fail_and_cleanup(L"Escenario 18: Fallo exportacion o validacion de archivo .reg real con reg.exe.", 184);
        }

        // 4. Registrar la operación en el manifiesto ANTES de modificar el registro
        ManifestOperation reg_op{};
        reg_op.type = OperationType::RestoreReg;
        reg_op.original_existed = true;
        reg_op.app_id = L"synth_reg_app";
        reg_op.reg_key_path = full_hkcu_key;
        reg_op.backup_host_path = valid_reg_path;
        std::wstring reg_op_id;
        if (!mgr.manifest().RecordOperationIntent(reg_op, reg_op_id).ok) {
            ::RegDeleteTreeW(HKEY_CURRENT_USER, synth_subkey.c_str());
            return fail_and_cleanup(L"Escenario 18: Fallo RecordOperationIntent para respaldo .reg valido.", 185);
        }

        // 5. Modificar la clave sintética (simulando la aplicación portable) y marcar APPLIED
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, synth_subkey.c_str(), 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS) {
            const wchar_t dirty_val[] = L"ModifiedByPortableApp";
            ::RegSetValueExW(hKey, L"Theme", 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(dirty_val),
                             static_cast<DWORD>(sizeof(dirty_val)));
            ::RegSetValueExW(hKey, L"ExtraPortableValue", 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(dirty_val),
                             static_cast<DWORD>(sizeof(dirty_val)));
            ::RegCloseKey(hKey);
        }
        mgr.manifest().UpdateOperationState(reg_op_id, OperationState::Applied);

        // 6. Durante la limpieza, eliminar la clave modificada e importar el respaldo .reg original
        RollbackHandlers handlers{};
        handlers.on_restore_reg = [&](const ManifestOperation& mop, DWORD& out_err) -> bool {
            ::RegDeleteTreeW(HKEY_CURRENT_USER, synth_subkey.c_str());
            if (!mop.original_existed) {
                out_err = ERROR_SUCCESS;
                return true;
            }
            DWORD imp_rc = 999;
            const std::wstring imp_cmd = L"reg.exe import \"" + mop.backup_host_path + L"\"";
            if (!SpawnChildAndWait(imp_cmd, imp_rc) || imp_rc != 0) {
                out_err = ERROR_GEN_FAILURE;
                return false;
            }
            out_err = ERROR_SUCCESS;
            return true;
        };

        auto rb = mgr.manifest().ExecutePendingRollbacks(handlers);

        // Verificar que el valor original fue restaurado, el valor extra desapareció y el .reg se limpió tras confirmar
        wchar_t restored_buf[128]{};
        DWORD buf_bytes = sizeof(restored_buf);
        LSTATUS q_theme = ERROR_GEN_FAILURE;
        LSTATUS q_extra = ERROR_SUCCESS;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, synth_subkey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            q_theme = ::RegQueryValueExW(hKey, L"Theme", nullptr, nullptr,
                                         reinterpret_cast<LPBYTE>(restored_buf), &buf_bytes);
            DWORD dummy_sz = 0;
            q_extra = ::RegQueryValueExW(hKey, L"ExtraPortableValue", nullptr, nullptr, nullptr, &dummy_sz);
            ::RegCloseKey(hKey);
        }
        ::RegDeleteTreeW(HKEY_CURRENT_USER, synth_subkey.c_str());

        if (!rb.all_succeeded || q_theme != ERROR_SUCCESS ||
            std::wstring_view(restored_buf) != L"HostOriginalValue_18" ||
            q_extra != ERROR_FILE_NOT_FOUND ||
            FileOrDirExists(valid_reg_path)) {
            return fail_and_cleanup(L"Escenario 18: La restauracion desde el respaldo .reg no dejo la clave en su estado original.", 186);
        }
        DWORD fin_err = 0;
        mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err);
    }

    // =========================================================================
    // ESCENARIO 19 y 22 (AUDITORÍA): Preservación de respaldos (.reg y .pmbkp) ante fallo de restauración
    // =========================================================================
    {
        SessionManager mgr(sandbox_root);
        auto init = mgr.InitializeSessionWithId(L"session_audit_19_preserve_reg_on_fail");
        if (!init.ok) {
            return fail_and_cleanup(L"Escenario 19: No se pudo crear sesion.", 190);
        }

        const std::wstring reg_bkp_file = init.session_dir + L"\\must_survive_failure.reg";
        AtomicFile::WriteUtf8Text(
            reg_bkp_file,
            MakeValidSyntheticRegContent(L"Software\\PortableManager\\Tests\\FailReg19", L"Important", L"KeepMe"));

        ManifestOperation reg_op{};
        reg_op.type = OperationType::RestoreReg;
        reg_op.original_existed = true;
        reg_op.reg_key_path = L"HKCU\\Software\\PortableManager\\Tests\\FailReg19";
        reg_op.backup_host_path = reg_bkp_file;
        std::wstring op_id;
        if (!mgr.manifest().RecordOperationIntent(reg_op, op_id).ok) {
            return fail_and_cleanup(L"Escenario 19: Fallo al registrar RESTORE_REG.", 191);
        }
        mgr.manifest().UpdateOperationState(op_id, OperationState::Applied);

        // Simular fallo durante la importación del .reg
        RollbackHandlers fail_handlers{};
        fail_handlers.on_restore_reg = [&](const ManifestOperation&, DWORD& out_err) -> bool {
            out_err = ERROR_ACCESS_DENIED;
            return false;
        };

        auto rb = mgr.manifest().ExecutePendingRollbacks(fail_handlers);
        const auto* op_after = mgr.manifest().FindOperation(op_id);
        DWORD fin_err = 0;
        const bool fin_allowed = mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err);

        if (rb.all_succeeded || rb.failed_count != 1 ||
            !op_after || op_after->state != OperationState::Failed ||
            mgr.manifest().session_state() == SessionState::Cleaned ||
            fin_allowed ||
            !FileOrDirExists(reg_bkp_file) ||
            !FileOrDirExists(init.manifest_path)) {
            return fail_and_cleanup(L"Escenario 19/22: El archivo .reg no se preservo o la sesion se marco CLEANED pese al fallo.", 192);
        }
    }

    // =========================================================================
    // ESCENARIO 20 (AUDITORÍA): Apertura de sesión activa por PMG (OpenExistingSession) sin depender del USB
    // =========================================================================
    {
        SessionManager pm_mgr(sandbox_root);
        auto init = pm_mgr.InitializeSessionWithId(L"session_audit_20_pmg_active");
        if (!init.ok) {
            return fail_and_cleanup(L"Escenario 20: No se pudo crear sesion.", 200);
        }

        const std::wstring work_dir = sandbox_root + L"\\Audit20_TempDir";
        ManifestOperation op_dir{};
        op_dir.type = OperationType::DeleteDir;
        op_dir.original_existed = false;
        op_dir.target_host_path = work_dir;

        ManifestOperation op_job{};
        op_job.type = OperationType::KillJob;
        op_job.job_name = L"Local\\PM_Job_Audit20";

        std::wstring id_dir, id_job;
        pm_mgr.manifest().RecordOperationIntent(op_dir, id_dir);
        ::CreateDirectoryW(work_dir.c_str(), nullptr);
        pm_mgr.manifest().UpdateOperationState(id_dir, OperationState::Applied);

        pm_mgr.manifest().RecordOperationIntent(op_job, id_job);
        pm_mgr.manifest().UpdateOperationState(id_job, OperationState::Applied);
        pm_mgr.TransitionState(SessionState::Active);

        // Lanzar proceso hijo simulando a PMG abriendo la sesión activa indicada por PM
        DWORD pmg_rc = 999;
        const std::wstring cmd = L"\"" + self_exe + L"\" --worker pmg_open_active \"" +
                                 sandbox_root + L"\" \"" + init.session_id + L"\"";
        if (!SpawnChildAndWait(cmd, pmg_rc) || pmg_rc != 0 ||
            FileOrDirExists(work_dir) || FileOrDirExists(init.manifest_path)) {
            return fail_and_cleanup(L"Escenario 20: PMG no pudo abrir y limpiar la sesion activa indicada por PM.", 201);
        }
    }

    // =========================================================================
    // ESCENARIO 21 (AUDITORÍA): Ausencia de recuperación automática de sesiones antiguas al iniciar PM
    // =========================================================================
    {
        const std::wstring orphan_root = sandbox_root + L"\\OrphanIsolationTest";
        ::CreateDirectoryW(orphan_root.c_str(), nullptr);

        // 1. Dejar una sesión antigua en estado ACTIVE con un directorio creado
        const std::wstring old_dir = orphan_root + L"\\OldSessionDir_MustNotBeAutoCleaned";
        std::wstring orphan_manifest_path;
        {
            SessionManager old_pm(orphan_root);
            auto old_init = old_pm.InitializeSessionWithId(L"orphan_old_session_01");
            orphan_manifest_path = old_init.manifest_path;
            ManifestOperation op{};
            op.type = OperationType::DeleteDir;
            op.original_existed = false;
            op.target_host_path = old_dir;
            std::wstring op_id;
            old_pm.manifest().RecordOperationIntent(op, op_id);
            ::CreateDirectoryW(old_dir.c_str(), nullptr);
            old_pm.manifest().UpdateOperationState(op_id, OperationState::Applied);
            old_pm.TransitionState(SessionState::Active);
        }

        // 2. Iniciar una nueva instancia de PM (InitializeNewSession): NO debe tocar ni limpiar la sesión antigua
        SessionManager new_pm(orphan_root);
        auto new_init = new_pm.InitializeNewSession();
        if (!new_init.ok) {
            return fail_and_cleanup(L"Escenario 21: Fallo InitializeNewSession.", 210);
        }

        // 3. Comprobar que DiscoverOrphanSessions es puramente pasiva y que old_dir y su manifiesto siguen intactos en ACTIVE
        const auto orphans = new_pm.DiscoverOrphanSessions();
        SessionManager check_old(orphan_root);
        auto open_old = check_old.OpenExistingSession(L"orphan_old_session_01");
        if (!open_old.ok ||
            check_old.manifest().session_state() != SessionState::Active ||
            !FileOrDirExists(old_dir) ||
            !FileOrDirExists(orphan_manifest_path) ||
            orphans.size() != 1 ||
            orphans[0] != L"orphan_old_session_01") {
            return fail_and_cleanup(L"Escenario 21: La sesion antigua fue alterada o limpiada automaticamente al iniciar PM.", 211);
        }
    }

    // =========================================================================
    // ESCENARIO 23 (AUDITORÍA): Ausencia de eliminación de sesiones con operaciones pendientes
    // =========================================================================
    {
        SessionManager mgr(sandbox_root);
        auto init = mgr.InitializeSessionWithId(L"session_audit_23_pending_ops");
        if (!init.ok) {
            return fail_and_cleanup(L"Escenario 23: Fallo al crear sesion.", 230);
        }

        ManifestOperation op{};
        op.type = OperationType::DeleteDir;
        op.original_existed = false;
        op.target_host_path = sandbox_root + L"\\Audit23_PendingDir";
        std::wstring op_id;
        mgr.manifest().RecordOperationIntent(op, op_id);

        // Con la operación en INTENT_RECORDED, ni TransitionState(Cleaned) ni FinalizeSessionAndCleanupOwnFiles deben permitir limpiar
        DWORD fin_err = 0;
        if (mgr.TransitionState(SessionState::Cleaned).ok ||
            mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err) ||
            !FileOrDirExists(init.manifest_path)) {
            return fail_and_cleanup(L"Escenario 23: Se permitio eliminar o marcar CLEANED una sesion con operacion en INTENT_RECORDED.", 231);
        }

        // Con la operación en APPLIED, tampoco debe permitirse
        mgr.manifest().UpdateOperationState(op_id, OperationState::Applied);
        if (mgr.TransitionState(SessionState::Cleaned).ok ||
            mgr.FinalizeSessionAndCleanupOwnFiles(true, fin_err) ||
            !FileOrDirExists(init.manifest_path)) {
            return fail_and_cleanup(L"Escenario 23: Se permitio eliminar o marcar CLEANED una sesion con operacion en APPLIED.", 232);
        }
    }

    // Verificar que el archivo ajeno en %TEMP% nunca fue eliminado
    const auto foreign_check = AtomicFile::ReadUtf8Text(foreign_temp_file);
    if (!foreign_check.ok || foreign_check.content != "UNRELATED_TEMP_FILE_MUST_SURVIVE") {
        return fail_and_cleanup(L"Seguridad: Un archivo temporal ajeno fue modificado o eliminado.", 85);
    }

    DeleteTreeForTestCleanup(sandbox_root);

    std::wprintf(L"[PASS] test_phase2_runtime (23 escenarios: 16 base + 7 auditoria final Fase 2: KILL_JOB primero, .reg backup, sesiones huerfanas, estados unificados) superado exitosamente.\n");
    return 0;
}
