#include "junctions/junction_manager.h"

#include "core/path_resolver.h"
#include "runtime/session_lock.h"

#include <cstddef>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string_view>
#include <vector>
#include <winioctl.h>

namespace pm::junctions {
namespace {

using core::PathResolver;
using runtime::ManifestManager;
using runtime::ManifestOperation;
using runtime::OperationState;
using runtime::OperationType;

struct CloseHandle {
    void operator()(void* handle) const { ::CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandle>;

constexpr std::wstring_view kBackupSuffix = L".pmbkp";
constexpr std::wstring_view kStagePrefix = L".pm-junction-";

bool Fail(DWORD& error, DWORD code = ::GetLastError()) {
    error = code;
    return false;
}

bool Equal(std::wstring_view a, std::wstring_view b) {
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                  b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool Within(std::wstring_view child, std::wstring_view parent) {
    return child.size() > parent.size() && child[parent.size()] == L'\\' &&
           Equal(child.substr(0, parent.size()), parent);
}

bool IsDrivePath(std::wstring_view path) {
    if (path.size() <= 3 || path.size() >= MAX_PATH ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path[1] != L':' || path[2] != L'\\') return false;
    const auto normalized = PathResolver::NormalizePmRelativePath(path.substr(3));
    return normalized.ok && normalized.resolved_path == path.substr(3);
}

std::wstring Parent(const std::wstring& path) {
    return path.substr(0, path.rfind(L'\\'));
}

std::wstring Stage(const ManifestOperation& op) {
    return Parent(op.target_host_path) + L"\\" + std::wstring(kStagePrefix) + op.id;
}

bool IsHexId(std::wstring_view value, std::size_t length) {
    if (value.size() != length) return false;
    for (const wchar_t c : value) {
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    }
    return true;
}

bool IsGuid(std::wstring_view value) {
    return value.size() == 36 && IsHexId(value.substr(0, 8), 8) &&
           value[8] == L'-' && IsHexId(value.substr(9, 4), 4) &&
           value[13] == L'-' && IsHexId(value.substr(14, 4), 4) &&
           value[18] == L'-' && IsHexId(value.substr(19, 4), 4) &&
           value[23] == L'-' && IsHexId(value.substr(24), 12);
}

bool IsMissing(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// Open the entry itself and deny deletion/writes by other handles while it is
// inspected or mutated. Missing entries are distinguished from access failures.
bool OpenEntry(const std::wstring& path, DWORD access, Handle& out, DWORD& error,
               DWORD sharing = FILE_SHARE_READ) {
    const HANDLE handle = ::CreateFileW(path.c_str(), access, sharing, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                       nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (IsMissing(::GetLastError())) {
            out.reset();
            return true;
        }
        return Fail(error);
    }
    out.reset(handle);
    return true;
}

bool ReadInfo(HANDLE handle, BY_HANDLE_FILE_INFORMATION& info, DWORD& error) {
    return ::GetFileInformationByHandle(handle, &info) || Fail(error);
}

bool PlainDirectory(const BY_HANDLE_FILE_INFORMATION& info) {
    return (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ==
           FILE_ATTRIBUTE_DIRECTORY;
}

std::wstring FileId(const BY_HANDLE_FILE_INFORMATION& info) {
    wchar_t text[25]{};
    std::swprintf(text, 25, L"%08lx%08lx%08lx", info.dwVolumeSerialNumber,
                  info.nFileIndexHigh, info.nFileIndexLow);
    return text;
}

// Pin every ancestor before descending. Reject ancestor junctions and aliases
// such as a short path or a substituted drive before using the full path.
bool PinPath(const std::wstring& path, std::vector<Handle>& chain, DWORD& error,
             DWORD sharing = FILE_SHARE_READ) {
    for (std::size_t end = 3;;) {
        const auto prefix = path.substr(0, end);
        Handle dir;
        if (!OpenEntry(prefix, FILE_READ_ATTRIBUTES, dir, error, sharing)) return false;
        if (!dir) return Fail(error, ERROR_PATH_NOT_FOUND);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!ReadInfo(dir.get(), info, error)) return false;
        if (!PlainDirectory(info)) return Fail(error, ERROR_DIRECTORY);
        chain.push_back(std::move(dir));
        if (end == path.size()) break;
        end = path.find(L'\\', end == 3 ? end : end + 1);
        if (end == std::wstring::npos) end = path.size();
    }

    wchar_t canonical[MAX_PATH + 5]{};
    const DWORD n = ::GetFinalPathNameByHandleW(chain.back().get(), canonical,
                                               std::size(canonical), FILE_NAME_NORMALIZED);
    if (!n) return Fail(error);
    if (n >= std::size(canonical) || !Equal(std::wstring_view(canonical, n), L"\\\\?\\" + path))
        return Fail(error, ERROR_INVALID_NAME);
    return true;
}

bool Rename(HANDLE source, const std::wstring& to, DWORD& error) {
    const auto name_bytes = to.size() * sizeof(wchar_t);
    // Storage is aligned for FILE_RENAME_INFO and includes the final NUL.
    auto buffer = std::make_unique<std::byte[]>(sizeof(FILE_RENAME_INFO) + name_bytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.get());
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(rename->FileName, to.c_str(), name_bytes + sizeof(wchar_t));
    return ::SetFileInformationByHandle(source, FileRenameInfo, rename,
                                       static_cast<DWORD>(sizeof(*rename) + name_bytes)) ||
           Fail(error);
}

struct MountPointData {
    DWORD tag;
    WORD data_length;
    WORD reserved;
    WORD substitute_offset;
    WORD substitute_length;
    WORD print_offset;
    WORD print_length;
    wchar_t path[2 * MAX_PATH + 8];
};
static_assert(offsetof(MountPointData, path) == 16);

bool SetMountPoint(HANDLE dir, const std::wstring& target, DWORD& error) {
    const auto substitute = L"\\??\\" + target;
    MountPointData data{};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.print_length = static_cast<WORD>(target.size() * sizeof(wchar_t));
    data.data_length = static_cast<WORD>(8 + data.print_offset + data.print_length + sizeof(wchar_t));
    std::memcpy(data.path, substitute.c_str(), data.print_offset);
    std::memcpy(data.path + data.print_offset / sizeof(wchar_t), target.c_str(),
                data.print_length + sizeof(wchar_t));
    DWORD returned = 0;
    return ::DeviceIoControl(dir, FSCTL_SET_REPARSE_POINT, &data, 8u + data.data_length,
                             nullptr, 0, &returned, nullptr) || Fail(error);
}

bool CheckMountPoint(HANDLE dir, const std::wstring& target, DWORD& error) {
    MountPointData data{};
    DWORD returned = 0;
    if (!::DeviceIoControl(dir, FSCTL_GET_REPARSE_POINT, nullptr, 0, &data,
                           sizeof(data), &returned, nullptr)) return Fail(error);
    const unsigned end = data.substitute_offset + data.substitute_length;
    if (data.tag != IO_REPARSE_TAG_MOUNT_POINT || returned < offsetof(MountPointData, path) ||
        data.data_length < 8 || returned != 8u + data.data_length || end > data.data_length - 8u ||
        end > sizeof(data.path) || data.substitute_offset % sizeof(wchar_t) != 0 ||
        data.substitute_length % sizeof(wchar_t) != 0)
        return Fail(error, ERROR_INVALID_REPARSE_DATA);
    const std::wstring_view actual(data.path + data.substitute_offset / sizeof(wchar_t),
                                   data.substitute_length / sizeof(wchar_t));
    return actual == L"\\??\\" + target || Fail(error, ERROR_REPARSE_ATTRIBUTE_CONFLICT);
}

bool RemoveOwnedEntry(Handle& dir, const BY_HANDLE_FILE_INFORMATION& info,
                      const ManifestOperation& op, const std::wstring& path, bool staging, DWORD& error) {
    if (op.junction_file_id != FileId(info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return Fail(error, ERROR_INVALID_DATA);
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (!CheckMountPoint(dir.get(), op.junction_target, error)) return false;
    } else if (!staging) {
        return Fail(error, ERROR_NOT_A_REPARSE_POINT);
    }
    // Bind deletion to the validated handle, with no opportunity to replace the
    // directory between checking the reparse data and deleting it by name.
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!::SetFileInformationByHandle(dir.get(), FileDispositionInfo, &disposition,
                                      sizeof(disposition))) return Fail(error);
    dir.reset();
    Handle remaining;
    if (!OpenEntry(path, FILE_READ_ATTRIBUTES, remaining, error,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)) return false;
    return !remaining || Fail(error, ERROR_ALREADY_EXISTS);
}

} // namespace

bool JunctionManager::IsJunctionOperation(const ManifestOperation& op) {
    std::wstring validation_error;
    if (op.type != OperationType::RemoveJunction || !IsGuid(op.id) ||
        !ManifestManager::ValidateOperationSecurity(op, validation_error) ||
        !IsDrivePath(op.target_host_path) || !IsDrivePath(op.junction_target) ||
        !IsDrivePath(Stage(op)) || !IsDrivePath(op.backup_host_path) ||
        !IsHexId(op.host_parent_file_id, 24) ||
        (!op.junction_file_id.empty() && !IsHexId(op.junction_file_id, 24)) ||
        (op.state == OperationState::Applied && op.junction_file_id.empty()) ||
        (op.original_existed ? !IsHexId(op.original_file_id, 24) : !op.original_file_id.empty()) ||
        op.backup_host_path != op.target_host_path + std::wstring(kBackupSuffix)) return false;
    return !Equal(op.target_host_path, op.junction_target) &&
           !Within(op.target_host_path, op.junction_target) &&
           !Within(op.junction_target, op.target_host_path);
}

bool JunctionManager::Prepare(runtime::SessionManager& session, const PathResolver& resolver,
                              const std::wstring& app_id, const std::wstring& exclusive_host_path,
                              const std::wstring& data_relative_path, DWORD& error) {
    error = ERROR_SUCCESS;
    const auto app = PathResolver::NormalizePmRelativePath(app_id);
    const auto relative = PathResolver::NormalizePmRelativePath(data_relative_path);
    const auto target = resolver.ResolvePmRelativePath(data_relative_path);
    const std::wstring app_data = L"Data\\apps\\" + app_id;
    if (!app.ok || app.resolved_path != app_id || app_id.find(L'\\') != std::wstring::npos ||
        !relative.ok || !target.ok ||
        (!Equal(relative.resolved_path, app_data) && !Within(relative.resolved_path, app_data)) ||
        !IsDrivePath(target.resolved_path) || !IsDrivePath(exclusive_host_path))
        return Fail(error, ERROR_INVALID_NAME);

    const auto leaf = std::wstring_view(exclusive_host_path).substr(exclusive_host_path.rfind(L'\\') + 1);
    if (Equal(exclusive_host_path, session.base_sessions_root()) ||
        Within(exclusive_host_path, session.base_sessions_root()) ||
        Within(session.base_sessions_root(), exclusive_host_path) ||
        Equal(exclusive_host_path, resolver.root_dir()) ||
        Within(exclusive_host_path, resolver.root_dir()) ||
        Within(resolver.root_dir(), exclusive_host_path) ||
        (leaf.size() >= kBackupSuffix.size() && Equal(leaf.substr(leaf.size() - kBackupSuffix.size()), kBackupSuffix)) ||
        (leaf.size() >= kStagePrefix.size() && Equal(leaf.substr(0, kStagePrefix.size()), kStagePrefix)))
        return Fail(error, ERROR_INVALID_NAME);

    runtime::SessionLock lock(session.session_mutex_name());
    if (!lock.ok()) return Fail(error, lock.error());
    auto& manifest = session.manifest();
    const auto loaded = manifest.LoadFromDisk();
    if (!loaded.ok) return Fail(error, loaded.win32_error);
    if (manifest.session_state() != runtime::SessionState::Preparing)
        return Fail(error, ERROR_INVALID_STATE);

    ManifestOperation op;
    op.id = runtime::SessionManager::GenerateSessionGuid();
    op.type = OperationType::RemoveJunction;
    op.app_id = app_id;
    op.target_host_path = exclusive_host_path;
    op.backup_host_path = exclusive_host_path + std::wstring(kBackupSuffix);
    op.junction_target = target.resolved_path;
    std::wstring validation_error;
    if (!ManifestManager::ValidateOperationSecurity(op, validation_error))
        return Fail(error, ERROR_INVALID_NAME);
    for (const auto& existing : manifest.operations()) {
        if (Equal(existing.target_host_path, exclusive_host_path) ||
            Within(existing.target_host_path, exclusive_host_path) ||
            Within(exclusive_host_path, existing.target_host_path))
            return Fail(error, ERROR_ALREADY_EXISTS);
    }

    std::vector<Handle> host_chain, target_chain;
    // Permit regular writes to the app data. The host chain and managed entries
    // are independently pinned for the duration of the transaction.
    if (!PinPath(Parent(exclusive_host_path), host_chain, error) ||
        !PinPath(op.junction_target, target_chain, error, FILE_SHARE_READ | FILE_SHARE_WRITE))
        return false;

    BY_HANDLE_FILE_INFORMATION info{};
    if (!ReadInfo(host_chain.back().get(), info, error)) return false;
    wchar_t filesystem[16]{};
    if (!::GetVolumeInformationByHandleW(host_chain.back().get(), nullptr, 0, nullptr,
                                         nullptr, nullptr, filesystem, std::size(filesystem)))
        return Fail(error);
    if (!Equal(filesystem, L"NTFS")) return Fail(error, ERROR_NOT_SUPPORTED);
    op.host_parent_file_id = FileId(info);
    Handle original, backup;
    if (!OpenEntry(op.backup_host_path, FILE_READ_ATTRIBUTES, backup, error) ||
        !OpenEntry(exclusive_host_path, FILE_READ_ATTRIBUTES | DELETE, original, error))
        return false;
    if (backup) return Fail(error, ERROR_ALREADY_EXISTS);
    if (original) {
        if (!ReadInfo(original.get(), info, error)) return false;
        if (!PlainDirectory(info)) return Fail(error, ERROR_DIRECTORY);
        wchar_t canonical[MAX_PATH + 5]{};
        const DWORD length = ::GetFinalPathNameByHandleW(original.get(), canonical,
                                                        std::size(canonical), FILE_NAME_NORMALIZED);
        if (!length) return Fail(error);
        if (length >= std::size(canonical) ||
            !Equal(std::wstring_view(canonical, length), L"\\\\?\\" + exclusive_host_path))
            return Fail(error, ERROR_INVALID_NAME);
        op.original_existed = true;
        op.original_file_id = FileId(info);
    }
    if (!IsJunctionOperation(op)) return Fail(error, ERROR_INVALID_NAME);

    const auto stage_path = Stage(op);
    std::wstring operation_id;
    const auto intent = manifest.RecordOperationIntent(op, operation_id);
    if (!intent.ok) return Fail(error, intent.win32_error);
    if (!::CreateDirectoryW(stage_path.c_str(), nullptr)) return Fail(error);
    Handle stage;
    if (!OpenEntry(stage_path, GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE, stage, error)) return false;
    if (!stage) return Fail(error, ERROR_FILE_NOT_FOUND);
    if (!ReadInfo(stage.get(), info, error)) return false;
    if (!PlainDirectory(info)) return Fail(error, ERROR_DIRECTORY);
    const auto identified = manifest.RecordJunctionFileId(operation_id, FileId(info));
    if (!identified.ok) return Fail(error, identified.win32_error);
    if (!SetMountPoint(stage.get(), op.junction_target, error) ||
        !CheckMountPoint(stage.get(), op.junction_target, error)) return false;
    if (original && !Rename(original.get(), op.backup_host_path, error)) return false;
    if (!Rename(stage.get(), exclusive_host_path, error)) return false;
    const auto applied = manifest.UpdateOperationState(operation_id, OperationState::Applied);
    return applied.ok || Fail(error, applied.win32_error);
}

bool JunctionManager::Rollback(const ManifestOperation& op, DWORD& error) {
    error = ERROR_SUCCESS;
    if (!IsJunctionOperation(op)) return Fail(error, ERROR_INVALID_DATA);
    std::vector<Handle> host_chain;
    if (!PinPath(Parent(op.target_host_path), host_chain, error)) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!ReadInfo(host_chain.back().get(), info, error)) return false;
    if (op.host_parent_file_id != FileId(info)) return Fail(error, ERROR_INVALID_DATA);

    const auto stage_path = Stage(op);
    Handle stage, host, backup;
    constexpr DWORD access = FILE_READ_ATTRIBUTES | DELETE;
    if (!OpenEntry(op.backup_host_path, access, backup, error) ||
        !OpenEntry(op.target_host_path, access, host, error) ||
        !OpenEntry(stage_path, access, stage, error)) return false;

    if (backup) {
        if (!ReadInfo(backup.get(), info, error)) return false;
        if (!op.original_existed || !PlainDirectory(info) || op.original_file_id != FileId(info))
            return Fail(error, ERROR_INVALID_DATA);
    }
    bool original_in_place = false;
    BY_HANDLE_FILE_INFORMATION host_info{};
    if (host) {
        if (!ReadInfo(host.get(), host_info, error)) return false;
        original_in_place = op.original_existed && PlainDirectory(host_info) &&
                            op.original_file_id == FileId(host_info);
        if (original_in_place == static_cast<bool>(backup) && op.original_existed)
            return Fail(error, ERROR_INVALID_DATA);
    } else if (op.original_existed && !backup) {
        return Fail(error, ERROR_FILE_NOT_FOUND);
    }

    // An unidentified staging entry cannot block restoration of an intact
    // backup, but the session must remain pending while that entry exists.
    DWORD stage_error = ERROR_SUCCESS;
    if (stage && (!ReadInfo(stage.get(), info, stage_error) ||
                  !RemoveOwnedEntry(stage, info, op, stage_path, true, stage_error))) {
        stage.reset();
    }
    if (host && !original_in_place &&
        !RemoveOwnedEntry(host, host_info, op, op.target_host_path, false, error))
        return false;
    if (backup && !Rename(backup.get(), op.target_host_path, error)) return false;
    return stage_error == ERROR_SUCCESS || Fail(error, stage_error);
}

} // namespace pm::junctions
