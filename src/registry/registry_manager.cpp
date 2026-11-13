#include "registry/registry_manager.h"

#include "jobs/job_manager.h"
#include "runtime/session_lock.h"

#include <bcrypt.h>
#include <memory>

namespace pm::registry {
namespace {
using runtime::ManifestManager;
using runtime::ManifestOperation;
using runtime::OperationType;
using runtime::SessionManager;

constexpr std::wstring_view kPrefix = L"HKCU\\Software\\";
constexpr std::wstring_view kLongHive = L"HKEY_CURRENT_USER";
constexpr DWORD kMaxBackupBytes = 16 * 1024 * 1024;

struct CloseKey {
    void operator()(HKEY key) const { ::RegCloseKey(key); }
};
using Key = std::unique_ptr<std::remove_pointer_t<HKEY>, CloseKey>;

struct CloseHandle {
    void operator()(HANDLE handle) const { ::CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandle>;

bool Fail(DWORD& error, DWORD code) {
    error = code;
    return false;
}

bool Missing(DWORD code) {
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

bool Equal(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                  static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool AtOrBelow(std::wstring_view key, std::wstring_view root) {
    return Equal(key, root) || (key.size() > root.size() && key[root.size()] == L'\\' &&
                                Equal(key.substr(0, root.size()), root));
}

bool PlainName(std::wstring_view name) {
    return !name.empty() && name.size() <= 255 && name != L"." && name != L".." &&
           name.front() != L' ' && name.back() != L' ' &&
           name.find_first_of(L"[]/\\\"<>|?*:") == std::wstring_view::npos &&
           name.find_first_of(std::wstring_view(L"\0\r\n\t", 4)) == std::wstring_view::npos;
}

bool SafeKey(std::wstring_view key) {
    if (key.size() <= kPrefix.size() || key.size() > 1024 ||
        !Equal(key.substr(0, kPrefix.size()), kPrefix)) return false;
    auto parts = key.substr(kPrefix.size());
    // Classes is shared with the merged HKCR view.
    if (AtOrBelow(parts, L"Classes")) return false;
    for (;;) {
        const auto end = parts.find(L'\\');
        if (!PlainName(parts.substr(0, end))) return false;
        if (end == std::wstring_view::npos) return true;
        parts.remove_prefix(end + 1);
    }
}

LSTATUS OpenPlainKey(HKEY parent, const std::wstring& name, REGSAM access, Key& key) {
    HKEY raw = nullptr;
    LSTATUS status = ::RegOpenKeyExW(parent, name.c_str(), REG_OPTION_OPEN_LINK,
                                     access | KEY_QUERY_VALUE | KEY_WOW64_64KEY, &raw);
    if (status != ERROR_SUCCESS) return status;
    key.reset(raw);
    DWORD type = REG_NONE;
    status = ::RegQueryValueExW(raw, L"SymbolicLinkValue", nullptr, &type, nullptr, nullptr);
    if (status == ERROR_SUCCESS) return type == REG_LINK ? ERROR_NOT_SUPPORTED : ERROR_SUCCESS;
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

LSTATUS CheckTree(HKEY key, unsigned depth = 0) {
    if (depth >= 512) return ERROR_INVALID_DATA;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]{};
        DWORD count = std::size(name);
        const LSTATUS status = ::RegEnumKeyExW(key, index, name, &count, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) return ERROR_SUCCESS;
        if (status != ERROR_SUCCESS) return status;
        if (!PlainName(std::wstring_view(name, count))) return ERROR_INVALID_NAME;
        Key child;
        const LSTATUS opened = OpenPlainKey(key, name, KEY_ENUMERATE_SUB_KEYS, child);
        if (opened != ERROR_SUCCESS) return opened;
        const LSTATUS checked = CheckTree(child.get(), depth + 1);
        if (checked != ERROR_SUCCESS) return checked;
    }
}

// Walk one component at a time so a link in an ancestor cannot redirect the
// HKCU path. Hold the checked parent for RegDeleteTreeW.
bool InspectKey(std::wstring_view path, bool deleting, Key& parent, Key& leaf,
                std::wstring& leaf_name, DWORD& error) {
    leaf_name.clear();
    path.remove_prefix(5); // Strip the hive prefix.
    HKEY current = HKEY_CURRENT_USER;
    for (;;) {
        const auto end = path.find(L'\\');
        const bool last = end == std::wstring_view::npos;
        std::wstring name(path.substr(0, end));
        if (last) leaf_name = name;
        Key next;
        const REGSAM access = KEY_ENUMERATE_SUB_KEYS |
            (deleting && !last && path.find(L'\\', end + 1) == std::wstring_view::npos
                 ? DELETE | KEY_SET_VALUE : 0);
        const LSTATUS status = OpenPlainKey(current, name, access, next);
        if (Missing(status)) return true;
        if (status != ERROR_SUCCESS) return Fail(error, status);
        if (last) {
            const LSTATUS checked = CheckTree(next.get());
            if (checked != ERROR_SUCCESS) return Fail(error, checked);
            leaf = std::move(next);
            return true;
        }
        parent = std::move(next);
        current = parent.get();
        path.remove_prefix(end + 1);
    }
}

bool RunReg(std::wstring_view args, DWORD& error) {
    wchar_t system[MAX_PATH]{};
    const UINT length = ::GetSystemDirectoryW(system, std::size(system));
    if (!length || length >= std::size(system)) return Fail(error, ERROR_PATH_NOT_FOUND);
    const std::wstring exe = std::wstring(system, length) + L"\\reg.exe";
    std::wstring command = jobs::QuoteArgument(exe) + L" " + std::wstring(args);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, system, &startup, &process)) return Fail(error, ::GetLastError());
    Handle child(process.hProcess), thread(process.hThread);
    const DWORD wait = ::WaitForSingleObject(child.get(), 30000);
    if (wait != WAIT_OBJECT_0) {
        const DWORD cause = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ::GetLastError();
        ::TerminateProcess(child.get(), cause);
        ::WaitForSingleObject(child.get(), INFINITE);
        return Fail(error, cause);
    }
    DWORD code = 0;
    if (!::GetExitCodeProcess(child.get(), &code)) return Fail(error, ::GetLastError());
    return code == 0 || Fail(error, ERROR_CAN_NOT_COMPLETE);
}

bool Export(const std::wstring& key, const std::wstring& file, DWORD& error) {
    // No /y: a pre-existing file must never be overwritten.
    return RunReg(L"export " + jobs::QuoteArgument(key) + L" " + jobs::QuoteArgument(file), error);
}

bool ScopedSections(std::wstring_view contents, const std::wstring& key) {
    constexpr std::wstring_view header = L"Windows Registry Editor Version 5.00\r\n";
    if (!contents.starts_with(header) || !contents.ends_with(L"\r\n") ||
        contents.find(L'\0') != std::wstring_view::npos) return false;
    contents.remove_prefix(header.size());
    const std::wstring root = std::wstring(kLongHive) + key.substr(4);
    bool found_root = false;
    while (!contents.empty()) {
        const auto end = contents.find(L"\r\n");
        if (end == std::wstring_view::npos) return false;
        auto line = contents.substr(0, end);
        contents.remove_prefix(end + 2);
        if (line.find_first_of(L"\r\n") != std::wstring_view::npos) return false;
        const auto first = line.find_first_not_of(L" \t");
        if (first == std::wstring_view::npos) continue;
        line.remove_prefix(first);
        if (line.front() != L'[') continue;
        if (line.size() < 3 || line.back() != L']') return false;
        const auto section = line.substr(1, line.size() - 2);
        if (!AtOrBelow(section, root)) return false;
        if (Equal(section, root)) found_root = true;
    }
    return found_root;
}

bool ReadBackup(const ManifestOperation& op, const std::wstring& file, DWORD access,
                Handle& pinned, std::wstring& digest, DWORD& error) {
    HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_READ | access, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Fail(error, ::GetLastError());
    pinned.reset(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info)) return Fail(error, ::GetLastError());
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ||
        info.nNumberOfLinks != 1 || info.nFileSizeHigh || info.nFileSizeLow < 4 ||
        info.nFileSizeLow > kMaxBackupBytes || info.nFileSizeLow % sizeof(wchar_t))
        return Fail(error, ERROR_INVALID_DATA);

    std::wstring contents(info.nFileSizeLow / sizeof(wchar_t), L'\0');
    DWORD count = 0;
    if (!::ReadFile(handle, contents.data(), info.nFileSizeLow, &count, nullptr))
        return Fail(error, ::GetLastError());
    if (count != info.nFileSizeLow || contents.front() != L'\xfeff' ||
        !ScopedSections(std::wstring_view(contents).substr(1), op.reg_key_path))
        return Fail(error, ERROR_INVALID_DATA);

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return Fail(error, ERROR_GEN_FAILURE);
    BCRYPT_HASH_HANDLE hash = nullptr;
    UCHAR bytes[32]{};
    const bool hashed = ::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
        ::BCryptHashData(hash, reinterpret_cast<PUCHAR>(contents.data()), count, 0) == 0 &&
        ::BCryptFinishHash(hash, bytes, sizeof(bytes), 0) == 0;
    if (hash) ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!hashed) return Fail(error, ERROR_GEN_FAILURE);
    constexpr wchar_t hex[] = L"0123456789abcdef";
    digest.clear();
    for (UCHAR byte : bytes) {
        digest.push_back(hex[byte >> 4]);
        digest.push_back(hex[byte & 15]);
    }
    return true;
}

bool PinSessionDirectory(const std::wstring& directory, Handle& pinned, DWORD& error) {
    HANDLE handle = ::CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Fail(error, ::GetLastError());
    pinned.reset(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info)) return Fail(error, ::GetLastError());
    return ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ||
           Fail(error, ERROR_INVALID_DATA);
}

std::wstring BackupPath(std::wstring_view dir, const ManifestOperation& op) {
    return std::wstring(dir) + L"\\registry-" + op.id + L".reg";
}

std::wstring BackupDirectory(const ManifestOperation& op) {
    return op.backup_host_path.substr(0, op.backup_host_path.rfind(L'\\'));
}

bool RemoveOpenedFile(HANDLE handle, DWORD& error) {
    FILE_DISPOSITION_INFO remove{TRUE};
    return ::SetFileInformationByHandle(handle, FileDispositionInfo, &remove, sizeof(remove)) ||
           Fail(error, ::GetLastError());
}

bool DiscardSessionExport(const std::wstring& path, DWORD& error) {
    HANDLE handle = ::CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        return Missing(code) || Fail(error, code);
    }
    Handle file(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info)) return Fail(error, ::GetLastError());
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ||
        info.nNumberOfLinks != 1) return Fail(error, ERROR_INVALID_DATA);
    return RemoveOpenedFile(handle, error);
}

bool VerifyRestored(const ManifestOperation& op, DWORD& error) {
    // The check export has a dedicated name. A retry can discard a partial
    // check from a prior interrupted attempt without touching the original.
    const auto path = BackupDirectory(op) + L"\\registry-check-" + op.id + L".reg";
    if (!DiscardSessionExport(path, error)) return false;
    Handle file;
    std::wstring digest;
    const bool verified = Export(op.reg_key_path, path, error) &&
                          ReadBackup(op, path, 0, file, digest, error);
    file.reset();
    DWORD discard_error = ERROR_SUCCESS;
    if (!DiscardSessionExport(path, discard_error) && verified) return Fail(error, discard_error);
    if (!verified) return false;
    return digest == op.registry_backup_sha256 || Fail(error, ERROR_INVALID_DATA);
}
}

bool RegistryManager::IsRegistryOperation(const ManifestOperation& op, std::wstring_view session_dir) {
    if (op.type != OperationType::RestoreReg || !SafeKey(op.reg_key_path) || op.id.empty() ||
        !op.target_host_path.empty()) return false;
    if (!op.original_existed) return op.backup_host_path.empty() && op.registry_backup_sha256.empty();
    return !session_dir.empty() && Equal(op.backup_host_path, BackupPath(session_dir, op)) &&
           op.registry_backup_sha256.size() == 64 &&
           op.registry_backup_sha256.find_first_not_of(L"0123456789abcdef") == std::wstring::npos;
}

bool RegistryManager::Prepare(SessionManager& session, std::wstring_view exclusive_key, DWORD& error,
                              bool require_existing_parent, std::wstring_view app_id) {
    error = ERROR_SUCCESS;
    if (!SafeKey(exclusive_key) || session.session_dir().empty()) return Fail(error, ERROR_INVALID_NAME);
    runtime::SessionLock lock(session.session_mutex_name());
    if (!lock.ok()) return Fail(error, lock.error());
    auto& manifest = session.manifest();
    const auto loaded = manifest.LoadFromDisk();
    if (!loaded.ok) return Fail(error, loaded.win32_error);
    if (manifest.session_state() != runtime::SessionState::Preparing &&
        (manifest.session_state() != runtime::SessionState::Active ||
         !SessionManager::IsValidSessionId(app_id))) return Fail(error, ERROR_INVALID_STATE);
    for (const auto& existing : manifest.operations()) {
        if (ManifestManager::IsTerminalAcceptableState(existing.state)) continue;
        if (existing.type == OperationType::KillJob && (app_id.empty() || existing.app_id == app_id))
            return Fail(error, ERROR_INVALID_STATE);
        if (existing.type == OperationType::RestoreReg &&
            (AtOrBelow(exclusive_key, existing.reg_key_path) || AtOrBelow(existing.reg_key_path, exclusive_key)))
            return Fail(error, ERROR_ALREADY_EXISTS);
    }

    ManifestOperation op;
    op.id = SessionManager::GenerateSessionGuid();
    if (op.id.empty()) return Fail(error, ERROR_GEN_FAILURE);
    op.type = OperationType::RestoreReg;
    op.app_id = app_id;
    op.reg_key_path = exclusive_key;
    Key parent, leaf;
    std::wstring leaf_name;
    if (!InspectKey(op.reg_key_path, false, parent, leaf, leaf_name, error)) return false;
    if (leaf_name.empty()) return Fail(error, ERROR_PATH_NOT_FOUND);
    if (require_existing_parent && !parent) return Fail(error, ERROR_PATH_NOT_FOUND);
    op.original_existed = static_cast<bool>(leaf);
    Handle directory, backup;
    if (op.original_existed) {
        if (!PinSessionDirectory(session.session_dir(), directory, error)) return false;
        op.backup_host_path = BackupPath(session.session_dir(), op);
        if (!Export(op.reg_key_path, op.backup_host_path, error)) return false;
        if (!ReadBackup(op, op.backup_host_path, 0, backup, op.registry_backup_sha256, error)) {
            backup.reset();
            DWORD discard_error = ERROR_SUCCESS;
            DiscardSessionExport(op.backup_host_path, discard_error);
            return false;
        }
    }
    std::wstring id;
    const auto intent = manifest.RecordOperationIntent(std::move(op), id);
    // The application makes the actual changes. A durable intent is sufficient
    // to cover an interruption at any point after the application is launched.
    return intent.ok || Fail(error, intent.win32_error);
}

bool RegistryManager::HasChanges(const SessionManager& session, const ManifestOperation& op,
                                 bool& changed, DWORD& error) {
    changed = false;
    error = ERROR_SUCCESS;
    if (!SessionManager::IsValidSessionId(op.id) ||
        !IsRegistryOperation(op, session.session_dir()))
        return Fail(error, ERROR_INVALID_DATA);

    Key parent, leaf;
    std::wstring leaf_name;
    if (!InspectKey(op.reg_key_path, false, parent, leaf, leaf_name, error)) return false;
    if (!op.original_existed || !leaf) {
        changed = op.original_existed != static_cast<bool>(leaf);
        return true;
    }

    Handle directory;
    if (!PinSessionDirectory(session.session_dir(), directory, error)) return false;
    const std::wstring check = session.session_dir() + L"\\registry-observe-" + op.id + L".reg";
    if (!DiscardSessionExport(check, error)) return false;
    Handle file;
    std::wstring digest;
    bool ok = Export(op.reg_key_path, check, error) &&
              ReadBackup(op, check, 0, file, digest, error);
    file.reset();
    DWORD discard_error = ERROR_SUCCESS;
    if (!DiscardSessionExport(check, discard_error) && ok)
        return Fail(error, discard_error);
    if (!ok) return false;
    changed = digest != op.registry_backup_sha256;
    return true;
}

bool RegistryManager::Rollback(const ManifestOperation& op, DWORD& error) {
    error = ERROR_SUCCESS;
    if (!IsRegistryOperation(op, BackupDirectory(op))) return Fail(error, ERROR_INVALID_DATA);
    Handle directory, backup;
    std::wstring digest;
    if (op.original_existed) {
        if (!PinSessionDirectory(BackupDirectory(op), directory, error) ||
            !ReadBackup(op, op.backup_host_path, 0, backup, digest, error)) return false;
        if (digest != op.registry_backup_sha256) return Fail(error, ERROR_INVALID_DATA);
    }
    Key parent, leaf;
    std::wstring leaf_name;
    if (!InspectKey(op.reg_key_path, true, parent, leaf, leaf_name, error)) return false;
    if (op.original_existed && leaf_name.empty()) return Fail(error, ERROR_PATH_NOT_FOUND);
    if (leaf) {
        leaf.reset();
        const LSTATUS deleted = ::RegDeleteTreeW(parent.get(), leaf_name.c_str());
        if (deleted != ERROR_SUCCESS && !Missing(deleted)) return Fail(error, deleted);
    }
    if (!op.original_existed) {
        if (!InspectKey(op.reg_key_path, false, parent, leaf, leaf_name, error)) return false;
        return !leaf || Fail(error, ERROR_INVALID_DATA);
    }
    return RunReg(L"import " + jobs::QuoteArgument(op.backup_host_path) + L" /reg:64", error) &&
           VerifyRestored(op, error);
}

bool RegistryManager::ReleaseBackup(const ManifestOperation& op, DWORD& error) {
    error = ERROR_SUCCESS;
    Handle directory, backup;
    std::wstring digest;
    if (!op.original_existed || !IsRegistryOperation(op, BackupDirectory(op)))
        return Fail(error, ERROR_INVALID_DATA);
    if (!PinSessionDirectory(BackupDirectory(op), directory, error)) return false;
    if (!ReadBackup(op, op.backup_host_path, DELETE, backup, digest, error)) {
        if (!Missing(error)) return false;
        error = ERROR_SUCCESS;
        return true;
    }
    if (digest != op.registry_backup_sha256) return Fail(error, ERROR_INVALID_DATA);
    return RemoveOpenedFile(backup.get(), error);
}

bool RegistryManager::ExportPortableData(std::wstring_view key, std::wstring_view reg_file, DWORD& error) {
    error = ERROR_SUCCESS;
    if (!SafeKey(key) || reg_file.empty()) return Fail(error, ERROR_INVALID_PARAMETER);
    const std::wstring key_str(key);
    const std::wstring out_str(reg_file);
    Key parent, leaf;
    std::wstring leaf_name;
    if (!InspectKey(key_str, false, parent, leaf, leaf_name, error)) return false;
    if (!leaf) {
        ::DeleteFileW(out_str.c_str());
        error = ERROR_SUCCESS;
        return true;
    }
    const std::wstring tmp_file = out_str + L".pmtmp";
    ::DeleteFileW(tmp_file.c_str());
    if (!Export(key_str, tmp_file, error)) {
        ::DeleteFileW(tmp_file.c_str());
        return false;
    }
    if (!::MoveFileExW(tmp_file.c_str(), out_str.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = ::GetLastError();
        ::DeleteFileW(tmp_file.c_str());
        return false;
    }
    return true;
}

bool RegistryManager::ImportPortableData(std::wstring_view key, std::wstring_view reg_file, DWORD& error) {
    error = ERROR_SUCCESS;
    if (!SafeKey(key) || reg_file.empty()) return Fail(error, ERROR_INVALID_PARAMETER);
    ManifestOperation scope{};
    scope.reg_key_path = std::wstring(key);
    Handle pinned;
    std::wstring digest;
    if (!ReadBackup(scope, std::wstring(reg_file), 0, pinned, digest, error)) return false;
    pinned.reset();
    return RunReg(L"import " + jobs::QuoteArgument(std::wstring(reg_file)) + L" /reg:64", error);
}
}
