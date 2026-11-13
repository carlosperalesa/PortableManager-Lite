#include "runtime/session_manager.h"

#include "core/atomic_file.h"
#include "core/config_manager.h"

#include <objbase.h>
#include <algorithm>
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

    AcquireLivePresence();
    res.ok = true;
    res.session_id = session_id_;
    res.session_dir = session_dir_;
    res.manifest_path = manifest_path_;
    return res;
}

bool SessionManager::AcquireLivePresence() {
    if (session_id_.empty()) return false;
    if (live_event_) return true;
    HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, LiveEventName(session_id_).c_str());
    if (!ev) return false;
    live_event_ = std::shared_ptr<void>(ev, [](HANDLE h) { if (h) ::CloseHandle(h); });
    return true;
}

bool SessionManager::IsSessionLive(std::wstring_view session_id) {
    if (session_id.empty()) return false;
    HANDLE ev = ::OpenEventW(SYNCHRONIZE, FALSE, LiveEventName(session_id).c_str());
    if (!ev) return false;
    ::CloseHandle(ev);
    return true;
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
        if (IsSessionLive(dir_name)) {
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

namespace {
bool RemoveOwnedManifestFile(const std::wstring& path, const std::string& header,
                             bool& foreign, DWORD& error) {
    HANDLE file = ::CreateFileW(path.c_str(), DELETE | GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = ::GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) return false;
        error = ERROR_SUCCESS;
        return true;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    DWORD read = 0;
    std::string actual(header.size(), '\0');
    error = ERROR_SUCCESS;
    if (!::GetFileInformationByHandle(file, &info)) {
        error = ::GetLastError();
    } else if (info.nNumberOfLinks != 1 ||
               (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        foreign = true;
    } else if (!::ReadFile(file, actual.data(), static_cast<DWORD>(actual.size()), &read, nullptr)) {
        error = ::GetLastError();
    } else if (read != header.size() || actual != header) {
        foreign = true;
    } else {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!::SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)))
            error = ::GetLastError();
    }
    ::CloseHandle(file);
    return error == ERROR_SUCCESS;
}
}

bool SessionManager::FinalizeSessionAndCleanupOwnFiles(
    bool remove_manifest_when_cleaned,
    DWORD& out_error) {

    out_error = ERROR_SUCCESS;
    if (session_dir_.empty()) {
        out_error = ERROR_INVALID_STATE;
        return false;
    }

    const DWORD attributes = ::GetFileAttributesW(session_dir_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        out_error = ::GetLastError();
        return false;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        out_error = ERROR_INVALID_DATA;
        return false;
    }
    if (!manifest_.AreAllOperationsTerminalAcceptable()) {
        out_error = ERROR_DS_BUSY;
        return false;
    }

    const auto header = core::IniDocument::WideToUtf8(ManifestManager::SessionHeader(session_id_));
    bool other_entries = false;
    WIN32_FIND_DATAW entry{};
    const HANDLE scan = ::FindFirstFileW((session_dir_ + L"\\*").c_str(), &entry);
    if (scan == INVALID_HANDLE_VALUE) {
        out_error = ::GetLastError();
        return false;
    }
    constexpr std::wstring_view temp_prefix = L"session.ini.pmtmp.";
    do {
        const std::wstring_view name(entry.cFileName);
        if (name == L"." || name == L".." || name == L"session.ini") continue;
        const auto suffix = name.starts_with(temp_prefix) ? name.substr(temp_prefix.size()) : L"";
        const bool atomic_name = std::count(suffix.begin(), suffix.end(), L'.') == 3 &&
            suffix.find_first_not_of(L"0123456789.") == std::wstring_view::npos &&
            !suffix.starts_with(L".") && !suffix.ends_with(L".") &&
            suffix.find(L"..") == std::wstring_view::npos;
        if (!atomic_name || (entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            other_entries = true;
            if (name.size() >= 4 && ::CompareStringOrdinal(name.data() + name.size() - 4, 4, L".reg", 4, TRUE) == CSTR_EQUAL)
                out_error = ERROR_DS_BUSY;
            continue;
        }
        const auto temp = session_dir_ + L"\\" + std::wstring(name);
        DWORD temp_error = ERROR_SUCCESS;
        if (!RemoveOwnedManifestFile(temp, header, other_entries, temp_error)) out_error = temp_error;
    } while (::FindNextFileW(scan, &entry));
    const DWORD scan_error = ::GetLastError();
    ::FindClose(scan);
    if (scan_error != ERROR_NO_MORE_FILES) out_error = scan_error;
    if (out_error != ERROR_SUCCESS) return false;

    if (manifest_.session_state() != SessionState::Cleaned) {
        const auto st = manifest_.SetSessionStateAndPersist(SessionState::Cleaned);
        if (!st.ok) {
            out_error = st.win32_error;
            return false;
        }
    }

    // Keep the terminal manifest if an unowned entry prevents removing the directory.
    if (remove_manifest_when_cleaned && !other_entries) {
        bool foreign_manifest = false;
        if (!RemoveOwnedManifestFile(manifest_path_, header, foreign_manifest, out_error)) return false;
        if (foreign_manifest) {
            out_error = ERROR_INVALID_DATA;
            return false;
        }
        if (!::RemoveDirectoryW(session_dir_.c_str())) {
            out_error = ::GetLastError();
            return out_error == ERROR_FILE_NOT_FOUND || out_error == ERROR_PATH_NOT_FOUND;
        }
    }
    return true;
}

} // namespace pm::runtime
