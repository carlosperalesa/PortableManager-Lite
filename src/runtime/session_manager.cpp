#include "runtime/session_manager.h"

#include "core/atomic_file.h"

#include <objbase.h>
#include <cwchar>
#include <cwctype>

namespace pm::runtime {

SessionManager::SessionManager()
    : base_sessions_root_(BuildDefaultSessionsRoot(QuerySystemTempDir())) {}

SessionManager::SessionManager(std::wstring_view custom_temp_root)
    : base_sessions_root_(BuildDefaultSessionsRoot(custom_temp_root)) {}

std::wstring SessionManager::QuerySystemTempDir() {
    wchar_t buf[MAX_PATH + 2]{};
    const DWORD len = ::GetTempPathW(MAX_PATH + 1, buf);
    if (len == 0 || len > MAX_PATH) {
        return L"";
    }
    std::wstring temp_dir(buf, len);
    while (temp_dir.size() > 3 && (temp_dir.back() == L'\\' || temp_dir.back() == L'/')) {
        temp_dir.pop_back();
    }
    return temp_dir;
}

std::wstring SessionManager::BuildDefaultSessionsRoot(std::wstring_view temp_root) {
    if (temp_root.empty()) {
        return L"";
    }
    std::wstring root(temp_root);
    while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) {
        root.pop_back();
    }
    if (root.back() != L'\\') {
        root.push_back(L'\\');
    }
    root.append(L"PortableManager\\Session");
    return root;
}

std::wstring SessionManager::GenerateSessionGuid() {
    GUID guid{};
    if (FAILED(::CoCreateGuid(&guid))) {
        return L"";
    }
    wchar_t buf[64]{};
    std::swprintf(
        buf, sizeof(buf) / sizeof(wchar_t),
        L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buf;
}

bool SessionManager::IsValidSessionId(std::wstring_view session_id) noexcept {
    if (session_id.empty() || session_id.size() > 128) {
        return false;
    }
    for (wchar_t ch : session_id) {
        if (!(std::iswalnum(ch) || ch == L'-' || ch == L'_')) {
            return false;
        }
    }
    return true;
}

SessionInitResult SessionManager::InitializeNewSession() {
    const std::wstring guid = GenerateSessionGuid();
    if (guid.empty()) {
        return {false, ERROR_GEN_FAILURE, L"", L"", L"", L"Fallo al generar GUID de sesion."};
    }
    return InitializeSessionWithId(guid);
}

SessionInitResult SessionManager::InitializeNewSession(std::wstring_view /*unused_pm_root_dir*/) {
    return InitializeNewSession();
}

SessionInitResult SessionManager::InitializeSessionWithId(
    std::wstring_view session_id,
    std::wstring_view /*unused_pm_root_dir*/) {
    return InitializeSessionWithId(session_id);
}

SessionInitResult SessionManager::InitializeSessionWithId(std::wstring_view session_id) {
    SessionInitResult res{};
    if (base_sessions_root_.empty()) {
        res.win32_error = ERROR_PATH_NOT_FOUND;
        res.error_message = L"No se pudo determinar el directorio %TEMP%.";
        return res;
    }
    if (!IsValidSessionId(session_id)) {
        res.win32_error = ERROR_INVALID_NAME;
        res.error_message = L"SessionID contiene caracteres invalidos.";
        return res;
    }

    session_id_.assign(session_id);
    session_dir_ = base_sessions_root_ + L"\\" + session_id_;
    manifest_path_ = session_dir_ + L"\\session.ini";

    DWORD dir_err = ERROR_SUCCESS;
    if (!pm::core::AtomicFile::EnsureParentDirectories(manifest_path_, dir_err)) {
        res.win32_error = dir_err;
        res.error_message = L"No se pudo crear el directorio de sesion en %TEMP%.";
        return res;
    }

    manifest_.Configure(session_id_, manifest_path_);
    const ManifestStatusResult init_st = manifest_.InitializeNew();
    if (!init_st.ok) {
        res.win32_error = init_st.win32_error;
        res.error_message = init_st.error_message;
        return res;
    }

    res.ok = true;
    res.session_id = session_id_;
    res.session_dir = session_dir_;
    res.manifest_path = manifest_path_;
    return res;
}

ManifestStatusResult SessionManager::OpenExistingSession(std::wstring_view session_id) {
    if (!IsValidSessionId(session_id)) {
        return {false, false, ERROR_INVALID_NAME, L"SessionID invalido."};
    }
    session_id_.assign(session_id);
    session_dir_ = base_sessions_root_ + L"\\" + session_id_;
    manifest_path_ = session_dir_ + L"\\session.ini";
    manifest_.Configure(session_id_, manifest_path_);
    return manifest_.LoadFromDisk();
}

std::vector<std::wstring> SessionManager::DiscoverOrphanSessions() const {
    // Función diagnóstica de solo lectura (funcionalidad futura):
    // Enumera carpetas de sesión cuyo estado no sea CLEANED sin ejecutar ningún cambio,
    // limpieza ni restauración automática.
    std::vector<std::wstring> orphans;
    if (base_sessions_root_.empty()) {
        return orphans;
    }

    const std::wstring pattern = base_sessions_root_ + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return orphans;
    }

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        const std::wstring_view dir_name(fd.cFileName);
        if (dir_name == L"." || dir_name == L"..") {
            continue;
        }
        if (!IsValidSessionId(dir_name)) {
            continue;
        }
        if (!session_id_.empty() && dir_name == session_id_) {
            continue;
        }

        const std::wstring candidate_manifest =
            base_sessions_root_ + L"\\" + std::wstring(dir_name) + L"\\session.ini";
        if (::GetFileAttributesW(candidate_manifest.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }

        ManifestManager inspector(dir_name, candidate_manifest);
        const ManifestStatusResult st = inspector.LoadFromDisk();
        if (!st.ok || inspector.session_state() != SessionState::Cleaned) {
            orphans.emplace_back(dir_name);
        }
    } while (::FindNextFileW(hFind, &fd));

    ::FindClose(hFind);
    return orphans;
}

ManifestStatusResult SessionManager::TransitionState(SessionState new_state) {
    return manifest_.SetSessionStateAndPersist(new_state);
}

ManifestStatusResult SessionManager::QueryCurrentStateFromDisk(SessionState& out_state) {
    const ManifestStatusResult st = manifest_.LoadFromDisk();
    if (!st.ok) {
        return st;
    }
    out_state = manifest_.session_state();
    return st;
}

bool SessionManager::FinalizeSessionAndCleanupOwnFiles(
    bool remove_manifest_when_cleaned,
    DWORD& out_error) {

    out_error = ERROR_SUCCESS;
    if (session_dir_.empty()) {
        out_error = ERROR_INVALID_STATE;
        return false;
    }

    // 1. Eliminar exclusivamente archivos temporales transitorios de escritura atómica (session.ini.pmtmp.*)
    const std::wstring pattern = session_dir_ + L"\\session.ini.pmtmp.*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                const std::wstring tmp_file = session_dir_ + L"\\" + fd.cFileName;
                ::DeleteFileW(tmp_file.c_str());
            }
        } while (::FindNextFileW(hFind, &fd));
        ::FindClose(hFind);
    }

    // 2. Comprobar que TODAS las operaciones estén en un estado terminal aceptable (ROLLED_BACK o SKIPPED)
    if (!manifest_.AreAllOperationsTerminalAcceptable()) {
        out_error = ERROR_DS_BUSY;
        return false;
    }

    // 3. Verificar que no quede ningún respaldo .reg pendiente de restaurar en la carpeta de sesión
    const std::wstring reg_pattern = session_dir_ + L"\\*.reg";
    WIN32_FIND_DATAW reg_fd{};
    HANDLE hRegFind = ::FindFirstFileW(reg_pattern.c_str(), &reg_fd);
    if (hRegFind != INVALID_HANDLE_VALUE) {
        ::FindClose(hRegFind);
        out_error = ERROR_DS_BUSY;
        return false;
    }

    if (manifest_.session_state() != SessionState::Cleaned) {
        const auto st = manifest_.SetSessionStateAndPersist(SessionState::Cleaned);
        if (!st.ok) {
            out_error = st.win32_error;
            return false;
        }
    }

    if (remove_manifest_when_cleaned) {
        if (::GetFileAttributesW(manifest_path_.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!::DeleteFileW(manifest_path_.c_str())) {
                out_error = ::GetLastError();
                return false;
            }
        }
        // RemoveDirectoryW solo elimina la carpeta si está completamente vacía
        ::RemoveDirectoryW(session_dir_.c_str());
    }

    return true;
}

} // namespace pm::runtime
