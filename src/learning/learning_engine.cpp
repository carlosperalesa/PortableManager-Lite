#include "learning/learning_engine.h"

#include "core/config_manager.h"
#include "guard/guard_process.h"
#include "jobs/job_manager.h"
#include "junctions/junction_manager.h"
#include "registry/registry_manager.h"
#include "runtime/session_lock.h"

#include <algorithm>
#include <cwchar>
#include <memory>
#include <vector>

namespace pm::learning {
namespace {
using core::PathResolver;
using registry::RegistryManager;
using runtime::OperationType;
using runtime::SessionManager;

struct CloseHandle {
    void operator()(HANDLE handle) const { ::CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandle>;
using PinnedDirectories = std::vector<Handle>;

bool Missing(DWORD error) {
    return error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND;
}

std::wstring Lower(std::wstring value) {
    for (auto& ch : value) {
        if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
    }
    return value;
}

bool SafeComponent(std::wstring_view text) {
    constexpr std::wstring_view allowed =
        L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ._-";
    if (text.empty() || text.size() > 80 || text.front() == L'.' || text.front() == L' ' ||
        text.back() == L'.' || text.back() == L' ' ||
        text.find_first_not_of(allowed) != std::wstring_view::npos)
        return false;
    const auto normalized = PathResolver::NormalizePmRelativePath(text);
    return normalized.ok && normalized.resolved_path == text;
}

bool SharedCompany(const std::wstring& company) {
    const auto lower = Lower(company);
    return lower == L"microsoft" || lower.starts_with(L"microsoft ") ||
           lower == L"windows" || lower.starts_with(L"windows ") ||
           lower == L"classes" || lower == L"policies" || lower == L"wow6432node";
}

bool DrivePath(std::wstring_view path) {
    return path.size() > 3 && path[1] == L':' && path[2] == L'\\';
}

std::wstring VersionValue(void* info, WORD language, WORD codepage, const wchar_t* name) {
    wchar_t field[80]{};
    if (std::swprintf(field, std::size(field), L"\\StringFileInfo\\%04x%04x\\%ls",
                      static_cast<unsigned>(language), static_cast<unsigned>(codepage), name) < 0)
        return {};
    void* value = nullptr;
    UINT length = 0;
    if (!::VerQueryValueW(info, field, &value, &length) || !value || length < 2 || length > 81)
        return {};
    const auto* text = static_cast<const wchar_t*>(value);
    if (text[length - 1] != L'\0') return {};
    return std::wstring(text, length - 1);
}

// Only create directories below the portable root. Pin the checked path while
// using it so a reparse point cannot replace a component during publication.
DWORD PinPortableDirectory(const PathResolver& resolver, std::wstring_view relative,
                           bool create, PinnedDirectories& pinned) {
    const auto target = resolver.ResolvePmRelativePath(relative);
    if (!target.ok || !DrivePath(target.resolved_path)) return ERROR_INVALID_NAME;
    const auto& path = target.resolved_path;
    const auto root_length = resolver.root_dir().size();
    for (std::size_t end = path.find(L'\\', 3);; end = path.find(L'\\', end + 1)) {
        const auto prefix = path.substr(0, end);
        if (create && prefix.size() > root_length &&
            !::CreateDirectoryW(prefix.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS)
            return ::GetLastError();
        Handle dir(::CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (dir.get() == INVALID_HANDLE_VALUE) {
            dir.release();
            return ::GetLastError();
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(dir.get(), &info)) return ::GetLastError();
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_DIRECTORY;
        pinned.push_back(std::move(dir));
        if (end == std::wstring::npos) return ERROR_SUCCESS;
    }
}

// A bounded metadata-only inventory. Do not read or copy file contents.
// Never traverse reparse-point children, shared paths, or arbitrary roots.
DWORD ScanOutput(const std::wstring& directory, const std::wstring& prefix,
                 std::vector<std::wstring>& entries, unsigned depth = 0) {
    if (depth >= 32) return ERROR_BUFFER_OVERFLOW;
    WIN32_FIND_DATAW entry{};
    HANDLE search = ::FindFirstFileW((directory + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        return code == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : code;
    }
    DWORD error = ERROR_SUCCESS;
    do {
        const std::wstring_view name(entry.cFileName);
        if (name == L"." || name == L"..") continue;
        if (name.find_first_of(L"\r\n=[]") != std::wstring_view::npos ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            error = ERROR_INVALID_DATA;
            break;
        }
        if (entries.size() >= 512) {
            error = ERROR_BUFFER_OVERFLOW;
            break;
        }
        const std::wstring relative = prefix.empty() ? std::wstring(name)
                                                      : prefix + L"\\" + std::wstring(name);
        const bool is_dir = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entries.push_back((is_dir ? L"DIR:" : L"FILE:") + relative);
        if (is_dir) {
            error = ScanOutput(directory + L"\\" + std::wstring(name), relative, entries, depth + 1);
            if (error != ERROR_SUCCESS) break;
        }
    } while (::FindNextFileW(search, &entry));
    if (error == ERROR_SUCCESS && ::GetLastError() != ERROR_NO_MORE_FILES)
        error = ::GetLastError();
    ::FindClose(search);
    return error;
}

DWORD ReserveDataDirectory(const PathResolver& resolver, const LearningRequest& request,
                           const LearningResult& result) {
    PinnedDirectories pinned;
    const auto parent = L"Data\\apps\\" + request.app_id;
    const DWORD error = PinPortableDirectory(resolver, parent, true, pinned);
    if (error != ERROR_SUCCESS) return error;
    const auto data = resolver.ResolvePmRelativePath(result.data_path);
    if (!data.ok) return ERROR_INVALID_NAME;
    return ::CreateDirectoryW(data.resolved_path.c_str(), nullptr) ? ERROR_SUCCESS : ::GetLastError();
}

struct Folder {
    const wchar_t* label;
    const std::wstring& token;
    std::wstring relative;
    bool approved = false;
    bool learned = false;
    std::vector<std::wstring> affected_entries;
};

DWORD Publish(const PathResolver& resolver, const LearningRequest& request,
              const Candidates& candidates, std::array<Folder, 2>& folders,
              bool registry_changed, LearningResult& result) {
    // A recipe contains cleanup metadata, never application settings.
    for (auto& folder : folders) {
        if (!folder.approved) continue;
        PinnedDirectories pinned;
        const DWORD error = PinPortableDirectory(resolver, folder.relative, false, pinned);
        if (error != ERROR_SUCCESS) return error;
        const DWORD scanned = ScanOutput(resolver.ResolvePmRelativePath(folder.relative).resolved_path,
                                         L"", folder.affected_entries);
        if (scanned != ERROR_SUCCESS) return scanned;
        folder.learned = !folder.affected_entries.empty();
    }
    if (!registry_changed &&
        std::none_of(folders.begin(), folders.end(), [](const Folder& folder) { return folder.learned; }))
        return ERROR_SUCCESS;

    runtime::SessionLock recipe_lock(L"Local\\PM_LearningRecipe_" + Lower(request.app_id));
    if (!recipe_lock.ok()) return recipe_lock.error();
    PinnedDirectories pinned;
    const DWORD error = PinPortableDirectory(resolver, L"Data\\recipes", true, pinned);
    if (error != ERROR_SUCCESS) return error;
    const auto recipe = L"Data\\recipes\\" + request.app_id + L".ini";
    const auto full_recipe = resolver.ResolvePmRelativePath(recipe);
    if (!full_recipe.ok) return ERROR_INVALID_NAME;
    const auto attrs = ::GetFileAttributesW(full_recipe.resolved_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const auto cause = ::GetLastError();
        if (!Missing(cause)) return cause;
    } else if (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        return ERROR_INVALID_DATA;
    }
    core::ConfigManager config(resolver);
    core::IniDocument doc;
    const auto loaded = config.LoadRelativeIni(recipe, doc);
    if (!loaded.ok) return loaded.win32_error;
    const auto section = L"Learning." + result.session_id;
    if (doc.HasSection(section)) return ERROR_ALREADY_EXISTS;
    std::wstring message;
    for (const auto& folder : folders) {
        if (!folder.learned) continue;
        const std::wstring label(folder.label);
        if (!doc.SetValue(section, label + L"Host", folder.token, message) ||
            !doc.SetValue(section, label + L"Cleanup", L"REMOVE_JUNCTION_RESTORE_ORIGINAL", message))
            return ERROR_INVALID_DATA;
        unsigned index = 0;
        for (const auto& affected : folder.affected_entries) {
            if (!doc.SetValue(section, label + L"Item" + std::to_wstring(++index),
                              affected, message))
                return ERROR_INVALID_DATA;
        }
    }
    if (registry_changed &&
        (!doc.SetValue(section, L"RegistryKey", candidates.registry, message) ||
         !doc.SetValue(section, L"RegistryCleanup", L"RESTORE_REG", message)))
        return ERROR_INVALID_DATA;
    if (!doc.SetValue(L"Learning", L"LatestSession", result.session_id, message))
        return ERROR_INVALID_DATA;
    const auto saved = config.SaveRelativeIniIfModified(recipe, doc);
    if (saved.ok) result.recipe_path = recipe;
    return saved.win32_error;
}
}

std::optional<Candidates> LearningEngine::Inspect(const std::wstring& executable) {
    if (!DrivePath(executable) || executable.find(L'\0') != std::wstring::npos) return std::nullopt;
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(executable.c_str(), &ignored);
    if (!size || size > 1024 * 1024) return std::nullopt;
    std::vector<unsigned char> info(size);
    if (!::GetFileVersionInfoW(executable.c_str(), 0, size, info.data())) return std::nullopt;
    struct Translation { WORD language; WORD codepage; };
    void* values = nullptr;
    UINT bytes = 0;
    if (!::VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", &values, &bytes) ||
        !values || !bytes || bytes % sizeof(Translation) || bytes / sizeof(Translation) > 64)
        return std::nullopt;

    Candidates result;
    const auto* translations = static_cast<const Translation*>(values);
    for (UINT index = 0; index < bytes / sizeof(Translation); ++index) {
        const auto& [language, codepage] = translations[index];
        auto company = VersionValue(info.data(), language, codepage, L"CompanyName");
        auto product = VersionValue(info.data(), language, codepage, L"ProductName");
        if (!SafeComponent(company) || !SafeComponent(product) || SharedCompany(company) ||
            (!result.company.empty() && (result.company != company || result.product != product)))
            return std::nullopt;
        result.company = std::move(company);
        result.product = std::move(product);
    }
    const auto suffix = L"\\" + result.company + L"\\" + result.product;
    result.roaming = L"%APPDATA%" + suffix;
    result.local = L"%LOCALAPPDATA%" + suffix;
    result.registry = L"HKCU\\Software" + suffix;
    return result;
}

LearningResult LearningEngine::Run(const PathResolver& resolver, const LearningRequest& request) {
    LearningResult result;
    const auto candidates = Inspect(request.executable);
    if (!candidates || !SafeComponent(request.app_id) ||
        !SessionManager::IsValidSessionId(request.app_id)) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    std::array<Folder, 2> folders{{
        {L"Roaming", candidates->roaming, {}, false, false, {}},
        {L"Local", candidates->local, {}, false, false, {}}
    }};
    bool registry_approved = false;
    bool approved = false;
    for (const auto& path : request.exclusive_paths) {
        if (path.empty()) continue;
        approved = true;
        if (path == candidates->roaming) folders[0].approved = true;
        else if (path == candidates->local) folders[1].approved = true;
        else if (path == candidates->registry) registry_approved = true;
        else {
            result.error = ERROR_INVALID_NAME;
            return result;
        }
    }
    if (!approved) {
        result.error = ERROR_ACCESS_DENIED;
        return result;
    }
    const auto lease_name = L"Global\\PM_Learning_" + Lower(candidates->company) + L"/" +
                            Lower(candidates->product);
    ::SetLastError(ERROR_SUCCESS);
    Handle reservation(::CreateEventW(nullptr, TRUE, FALSE, lease_name.c_str()));
    const DWORD lease_error = ::GetLastError();
    if (!reservation || lease_error == ERROR_ALREADY_EXISTS) {
        result.error = lease_error;
        return result;
    }

    SessionManager session;
    const auto init = session.InitializeNewSession();
    if (!init.ok) {
        result.error = init.win32_error;
        return result;
    }
    result.session_id = init.session_id;
    result.data_path = L"Data\\apps\\" + request.app_id + L"\\" + result.session_id;
    const auto origin = PathResolver::QueryExecutablePath();
    guard::GuardProcess guard;
    if (!guard.Start(origin, session, result.error, reservation.get())) return result;

    jobs::JobManager job;
    bool registry_changed = false;
    const bool uses_folders = folders[0].approved || folders[1].approved;
    const auto run = [&]() -> DWORD {
        if (uses_folders) {
            const DWORD error = ReserveDataDirectory(resolver, request, result);
            if (error != ERROR_SUCCESS) return error;
        }
        for (auto& folder : folders) {
            if (!folder.approved) continue;
            const auto host = PathResolver::ResolveHostTokenPath(folder.token);
            if (!host.ok || !DrivePath(host.resolved_path)) return ERROR_INVALID_NAME;
            folder.relative = result.data_path + L"\\" + Lower(folder.label);
            PinnedDirectories pinned;
            const auto created = PinPortableDirectory(resolver, folder.relative, true, pinned);
            if (created != ERROR_SUCCESS) return created;
            DWORD error = ERROR_SUCCESS;
            if (!junctions::JunctionManager::Prepare(session, resolver, request.app_id,
                                                    host.resolved_path, folder.relative, error))
                return error;
        }
        DWORD error = ERROR_SUCCESS;
        if (registry_approved && !RegistryManager::Prepare(session, candidates->registry, error, true))
            return error;
        if (!job.Create(session, request.app_id, error)) return error;
        const auto active = session.TransitionState(runtime::SessionState::Active);
        if (!active.ok) return active.win32_error;
        PROCESS_INFORMATION process{};
        const auto cwd = request.executable.substr(0, request.executable.rfind(L'\\'));
        if (!job.Launch(request.executable, request.arguments, cwd, process, error)) return error;
        Handle child(process.hProcess), thread(process.hThread);
        const auto waited = guard::WaitForReleaseOrOriginLoss(origin, child.get(), request.timeout_ms, error);
        if (waited == guard::OriginWaitResult::OriginLost) return ERROR_DEVICE_NOT_CONNECTED;
        if (waited == guard::OriginWaitResult::TimedOut) return ERROR_TIMEOUT;
        if (waited != guard::OriginWaitResult::Released) return error;
        if (!jobs::JobManager::TerminateAndWait(job.name(), error)) return error;
        DWORD code = 0;
        if (!::GetExitCodeProcess(child.get(), &code)) return ::GetLastError();
        if (code) return ERROR_PROCESS_ABORTED;
        if (registry_approved) {
            for (const auto& op : session.manifest().operations()) {
                if (op.type == OperationType::RestoreReg &&
                    !RegistryManager::HasChanges(session, op, registry_changed, error))
                    return error;
            }
        }
        return ERROR_SUCCESS;
    };
    result.error = run();
    result.host_restored = guard::CleanupSession(session, result.rollback_error);
    if (!result.host_restored || result.error != ERROR_SUCCESS) return result;
    if (!guard::IsOriginAccessible(origin)) {
        result.error = ERROR_DEVICE_NOT_CONNECTED;
        return result;
    }
    result.error = Publish(resolver, request, *candidates, folders, registry_changed, result);
    return result;
}
}
