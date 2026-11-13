#include "learning/learning_engine.h"

#include "applications/session_host.h"
#include "core/config_manager.h"
#include "guard/guard_supervisor.h"
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
    const auto tail = std::wstring_view(path).substr(3);
    const auto normalized = PathResolver::NormalizePmRelativePath(tail);
    if (!normalized.ok || normalized.resolved_path != tail) return ERROR_INVALID_NAME;
    const auto root_length = resolver.root_dir().size();
    for (std::size_t end = path.find(L'\\', 3);; end = path.find(L'\\', end + 1)) {
        const auto prefix = path.substr(0, end);
        if (create && prefix.size() > root_length &&
            !::CreateDirectoryW(prefix.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS)
            return ::GetLastError();
        // MoveFileExW needs write sharing on the directory for atomic
        // publication. Withhold delete sharing to pin each checked component.
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

// Reject alternate path spellings that could hide traversal or an unsafe
// component before checking the real directory chain with Win32 handles.
bool IsCanonicalRelative(std::wstring_view path) {
    const auto normalized = PathResolver::NormalizePmRelativePath(path);
    return normalized.ok && normalized.resolved_path == path;
}

DWORD InspectInstallation(const PathResolver& resolver, const LearningRequest& request,
                          const std::wstring& app_dir, std::wstring& executable,
                          std::optional<Candidates>& candidates, std::wstring& fail_substep) {
    const auto installation = resolver.ResolvePmRelativePath(app_dir);
    if (!installation.ok || !IsCanonicalRelative(app_dir)) { fail_substep = L"ResolveAppDir"; return ERROR_INVALID_NAME; }
    const auto prefix = installation.resolved_path + L"\\";
    executable = request.executable;
    std::replace(executable.begin(), executable.end(), L'/', L'\\');
    if (executable.size() <= prefix.size() ||
        ::CompareStringOrdinal(executable.data(), static_cast<int>(prefix.size()),
                               prefix.data(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) {
        fail_substep = L"MatchExePrefix";
        return ERROR_INVALID_NAME;
    }

    const auto relative_exe = app_dir + L"\\" + executable.substr(prefix.size());
    if (!IsCanonicalRelative(relative_exe)) { fail_substep = L"CanonicalExe"; return ERROR_INVALID_NAME; }
    executable = resolver.ResolvePmRelativePath(relative_exe).resolved_path;
    PinnedDirectories pinned;
    const auto directory = relative_exe.substr(0, relative_exe.rfind(L'\\'));
    const DWORD pinned_error = PinPortableDirectory(resolver, directory, false, pinned);
    if (pinned_error != ERROR_SUCCESS) { fail_substep = L"PinPortableDirectory"; return pinned_error; }

    HANDLE raw = ::CreateFileW(executable.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) { fail_substep = L"OpenExe"; return ::GetLastError(); }
    const Handle file(raw);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(file.get(), &info)) { fail_substep = L"ExeFileInfo"; return ::GetLastError(); }
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        fail_substep = L"ExeAttributes";
        return ERROR_INVALID_DATA;
    }
    candidates = LearningEngine::Inspect(executable);
    if (!candidates && request.allow_fallback) {
        candidates = LearningEngine::InspectWithFallback(executable, request.app_id);
    }
    if (!candidates) { fail_substep = L"InspectCandidates"; return ERROR_INVALID_PARAMETER; }
    return ERROR_SUCCESS;
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

DWORD Publish(const PathResolver& resolver, const std::wstring& app_dir,
              const Candidates& candidates, std::array<Folder, 2>& folders,
              bool registry_changed, bool allow_clean_marker, LearningResult& result) {
    // A wrapper contains cleanup metadata only.
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
        std::none_of(folders.begin(), folders.end(), [](const Folder& folder) { return folder.learned; })) {
        if (!allow_clean_marker) return ERROR_SUCCESS;
    }

    PinnedDirectories pinned;
    const DWORD error = PinPortableDirectory(resolver, app_dir, false, pinned);
    if (error != ERROR_SUCCESS) return error;

    // Serialize writers to the same directory even if the app IDs or path
    // spellings differ. Hold the checked directory chain through publication.
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(pinned.back().get(), &info)) return ::GetLastError();
    const auto identity = std::to_wstring(info.dwVolumeSerialNumber) + L"_" +
        std::to_wstring(info.nFileIndexHigh) + L"_" + std::to_wstring(info.nFileIndexLow);
    runtime::SessionLock lock(L"Global\\PML_Wrapper_" + identity);
    if (!lock.ok()) return lock.error();

    const auto wrapper = app_dir + L"\\wrapper.ini";
    const auto full_wrapper = resolver.ResolvePmRelativePath(wrapper);
    const auto loaded = core::AtomicFile::ReadUtf8Text(full_wrapper.resolved_path, true);
    if (!loaded.ok && loaded.win32_error != ERROR_FILE_NOT_FOUND) return loaded.win32_error;
    core::IniDocument doc;
    std::wstring message;
    if (!doc.ParseUtf8(loaded.content, message)) return ERROR_INVALID_DATA;
    const auto section = L"Learning." + result.session_id;
    if (doc.HasSection(section)) return ERROR_ALREADY_EXISTS;
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
    const auto saved = core::AtomicFile::WriteUtf8Text(full_wrapper.resolved_path, doc.SerializeUtf8());
    if (saved.ok) result.wrapper_path = wrapper;
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

Candidates LearningEngine::InspectWithFallback(const std::wstring& executable, std::wstring_view app_id) {
    if (auto strict = Inspect(executable)) return *strict;

    auto sanitize = [](std::wstring_view raw) -> std::wstring {
        constexpr std::wstring_view allowed =
            L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ._-";
        std::wstring out;
        for (const wchar_t ch : raw) {
            if (allowed.find(ch) != std::wstring_view::npos) out.push_back(ch);
        }
        while (!out.empty() && (out.front() == L'.' || out.front() == L' ')) out.erase(out.begin());
        while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
        if (out.size() > 80) out.resize(80);
        return SafeComponent(out) ? out : std::wstring{};
    };

    std::wstring company, product;
    DWORD ignored = 0;
    const DWORD size = DrivePath(executable) ? ::GetFileVersionInfoSizeW(executable.c_str(), &ignored) : 0;
    if (size && size <= 1024 * 1024) {
        std::vector<unsigned char> info(size);
        struct Translation { WORD language; WORD codepage; };
        void* values = nullptr;
        UINT bytes = 0;
        if (::GetFileVersionInfoW(executable.c_str(), 0, size, info.data()) &&
            ::VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", &values, &bytes) &&
            values && bytes >= sizeof(Translation)) {
            const auto& [language, codepage] = *static_cast<const Translation*>(values);
            company = sanitize(VersionValue(info.data(), language, codepage, L"CompanyName"));
            product = sanitize(VersionValue(info.data(), language, codepage, L"ProductName"));
        }
    }

    auto fallback = sanitize(app_id);
    if (fallback.empty()) fallback = L"portable_app";
    if (company.empty() || SharedCompany(company)) company = fallback;
    if (product.empty()) product = fallback;

    Candidates result;
    result.company = std::move(company);
    result.product = std::move(product);
    const auto suffix = L"\\" + result.company + L"\\" + result.product;
    result.roaming = L"%APPDATA%" + suffix;
    result.local = L"%LOCALAPPDATA%" + suffix;
    result.registry = L"HKCU\\Software" + suffix;
    return result;
}

void LogLearningError(const PathResolver& resolver, std::wstring_view app_id,
                      std::wstring_view step, DWORD error) {
    if (!error) return;
    const auto logs_dir = resolver.ResolvePmRelativePath(L"Data\\logs");
    if (!logs_dir.ok) return;
    ::CreateDirectoryW(logs_dir.resolved_path.c_str(), nullptr);
    const std::wstring safe_id = app_id.empty() ? L"app" : std::wstring(app_id);
    const std::wstring log_path = logs_dir.resolved_path + L"\\learning_" + safe_id + L".log";
    const std::string line = "[LearningEngine] app=" + core::IniDocument::WideToUtf8(safe_id) +
        " step=" + core::IniDocument::WideToUtf8(step) +
        " win32=" + std::to_string(error) + "\n";
    core::AtomicFile::WriteUtf8Text(log_path, line);
}

LearningResult LearningEngine::Run(const PathResolver& resolver, const LearningRequest& request) {
    LearningResult result;
    if (!SafeComponent(request.app_id) || !SessionManager::IsValidSessionId(request.app_id)) {
        result.error = ERROR_INVALID_PARAMETER;
        LogLearningError(resolver, request.app_id, L"ValidateAppId", result.error);
        return result;
    }
    auto app_dir = request.app_dir;
    std::replace(app_dir.begin(), app_dir.end(), L'/', L'\\');
    std::wstring executable;
    std::optional<Candidates> candidates;
    std::wstring inspect_substep = L"InspectInstallation";
    result.error = InspectInstallation(resolver, request, app_dir, executable, candidates, inspect_substep);
    if (result.error != ERROR_SUCCESS) {
        LogLearningError(resolver, request.app_id, L"InspectInstallation." + inspect_substep, result.error);
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

    std::unique_ptr<SessionManager> owned_session;
    std::unique_ptr<guard::GuardSupervisor> owned_guard;
    if (!request.shared_host) {
        owned_session = std::make_unique<SessionManager>();
        const auto init = owned_session->InitializeNewSession();
        if (!init.ok) {
            result.error = init.win32_error;
            return result;
        }
    }
    SessionManager& session = request.shared_host ? request.shared_host->session() : *owned_session;
    result.session_id = session.session_id();
    result.data_path = L"Data\\apps\\" + request.app_id + L"\\" + result.session_id;
    const auto origin = request.shared_host ? request.shared_host->origin() : PathResolver::QueryExecutablePath();
    const HANDLE reservations[] = {reservation.get(), request.app_reservation};
    if (!request.shared_host) {
        owned_guard = std::make_unique<guard::GuardSupervisor>();
        owned_guard->Start(origin, session);
        result.error = owned_guard->Protect(request.app_id, reservations);
        if (result.error != ERROR_SUCCESS) {
            LogLearningError(resolver, request.app_id, L"GuardProtect", result.error);
            owned_guard->StopMonitoring();
            const auto restored = cleanup::CleanupEngine::Run(
                session, cleanup::Mode::Critical, cleanup::Artifacts::Remove, &owned_guard->process());
            result.host_restored = restored.ok;
            result.rollback_error = restored.error;
            return result;
        }
    }

    auto job = std::make_shared<jobs::JobManager>();
    bool added_to_host = false;
    bool registry_changed = false;
    std::wstring fail_step = L"Run";
    std::vector<std::wstring> created_host_parents;
    const bool uses_folders = folders[0].approved || folders[1].approved;
    const auto run = [&]() -> DWORD {
        if (uses_folders) {
            const DWORD error = ReserveDataDirectory(resolver, request, result);
            if (error != ERROR_SUCCESS) { fail_step = L"ReserveDataDirectory"; return error; }
        }
        std::unique_ptr<runtime::SessionLock> host_lock;
        if (request.shared_host) {
            host_lock = std::make_unique<runtime::SessionLock>(session.session_mutex_name());
            if (!host_lock->ok()) { fail_step = L"SessionLock"; return host_lock->error(); }
            const DWORD add_err = request.shared_host->Add(request.app_id, job, reservations, true);
            if (add_err != ERROR_SUCCESS) { fail_step = L"SessionHostAdd"; return add_err; }
            added_to_host = true;
        }
        for (auto& folder : folders) {
            if (!folder.approved) continue;
            const auto host = PathResolver::ResolveHostTokenPath(folder.token);
            if (!host.ok || !DrivePath(host.resolved_path)) { fail_step = L"ResolveHostToken"; return ERROR_INVALID_NAME; }
            const auto slash = host.resolved_path.rfind(L'\\');
            if (slash != std::wstring::npos && slash > 3) {
                const auto host_parent = host.resolved_path.substr(0, slash);
                if (::GetFileAttributesW(host_parent.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    runtime::ManifestOperation dir_op{};
                    dir_op.type = OperationType::DeleteDir;
                    dir_op.app_id = request.app_id;
                    dir_op.target_host_path = host_parent;
                    std::wstring op_id;
                    if (session.manifest().RecordOperationIntent(dir_op, op_id).ok) {
                        if (::CreateDirectoryW(host_parent.c_str(), nullptr)) {
                            session.manifest().UpdateOperationState(op_id, runtime::OperationState::Applied);
                            created_host_parents.push_back(host_parent);
                        } else {
                            session.manifest().UpdateOperationState(op_id, runtime::OperationState::Skipped);
                        }
                    }
                }
            }
            folder.relative = result.data_path + L"\\" + Lower(folder.label);
            PinnedDirectories pinned;
            const auto created = PinPortableDirectory(resolver, folder.relative, true, pinned);
            if (created != ERROR_SUCCESS) { fail_step = L"PinPortableDirectory"; return created; }
            DWORD error = ERROR_SUCCESS;
            if (!junctions::JunctionManager::Prepare(session, resolver, request.app_id,
                                                    host.resolved_path, folder.relative, error)) {
                fail_step = L"JunctionPrepare";
                return error;
            }
        }
        DWORD error = ERROR_SUCCESS;
        if (registry_approved && !RegistryManager::Prepare(session, candidates->registry, error, true)) {
            fail_step = L"RegistryPrepare";
            return error;
        }
        if (!job->Create(session, request.app_id, error)) { fail_step = L"JobCreate"; return error; }
        if (!request.shared_host) {
            const auto active = session.TransitionState(runtime::SessionState::Active);
            if (!active.ok) { fail_step = L"TransitionActive"; return active.win32_error; }
        }
        PROCESS_INFORMATION process{};
        const auto cwd = executable.substr(0, executable.rfind(L'\\'));
        if (!job->Launch(executable, request.arguments, cwd, process, error)) { fail_step = L"JobLaunch"; return error; }
        host_lock.reset();
        Handle child(process.hProcess), thread(process.hThread);
        const ULONGLONG deadline = request.timeout_ms == INFINITE
            ? 0 : (::GetTickCount64() + request.timeout_ms);
        auto waited = guard::WaitForReleaseOrOriginLoss(
            origin, child.get(), request.timeout_ms, error, request.stop_event);
        if (waited == guard::OriginWaitResult::OriginLost) return ERROR_DEVICE_NOT_CONNECTED;
        if (waited == guard::OriginWaitResult::TimedOut) return ERROR_TIMEOUT;
        if (waited != guard::OriginWaitResult::Released) return error;
        DWORD code = 0;
        if (!::GetExitCodeProcess(child.get(), &code)) return ::GetLastError();
        while (!jobs::JobManager::IsEmpty(job->name())) {
            DWORD slice = 200;
            if (request.timeout_ms != INFINITE) {
                const ULONGLONG now = ::GetTickCount64();
                if (now >= deadline) return ERROR_TIMEOUT;
                slice = static_cast<DWORD>(std::min<ULONGLONG>(200, deadline - now));
            }
            waited = guard::WaitForReleaseOrOriginLoss(origin, nullptr, slice, error, request.stop_event);
            if (waited == guard::OriginWaitResult::OriginLost) return ERROR_DEVICE_NOT_CONNECTED;
            if (waited == guard::OriginWaitResult::Failed) return error;
        }
        if (!request.shared_host) {
            if (!jobs::JobManager::TerminateAndWait(job->name(), error)) return error;
        }
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
    try {
        result.error = run();
    } catch (...) {
        result.error = ERROR_UNHANDLED_EXCEPTION;
    }
    if (request.shared_host) {
        if (result.error == ERROR_DEVICE_NOT_CONNECTED) request.shared_host->BeginShutdown();
        if (added_to_host) {
            runtime::SessionLock lock(session.session_mutex_name());
            if (!lock.ok()) {
                result.host_restored = false;
                result.rollback_error = lock.error();
            } else if (!request.shared_host->closing()) {
                const auto mode = result.error ? cleanup::Mode::Critical : cleanup::Mode::Normal;
                const auto cleaned = cleanup::CleanupEngine::RunApplication(session, request.app_id, mode);
                result.host_restored = cleaned.ok;
                result.rollback_error = cleaned.error;
                if (cleaned.ok) {
                    job.reset();
                    request.shared_host->Release(request.app_id);
                }
            } else {
                result.host_restored = true;
            }
        } else {
            result.host_restored = true;
        }
    } else {
        owned_guard->StopMonitoring();
        const auto restored = cleanup::CleanupEngine::Run(
            session, result.error == ERROR_SUCCESS ? cleanup::Mode::Normal : cleanup::Mode::Critical,
            cleanup::Artifacts::Remove, &owned_guard->process());
        result.host_restored = restored.ok;
        result.rollback_error = restored.error;
    }
    for (auto it = created_host_parents.rbegin(); it != created_host_parents.rend(); ++it) {
        ::RemoveDirectoryW(it->c_str()); // removes only if empty
    }
    if (!result.host_restored || result.error != ERROR_SUCCESS) {
        LogLearningError(resolver, request.app_id, fail_step, result.error ? result.error : result.rollback_error);
        return result;
    }
    if (!guard::IsOriginAccessible(origin)) {
        result.error = ERROR_DEVICE_NOT_CONNECTED;
        return result;
    }
    result.error = Publish(resolver, app_dir, *candidates, folders, registry_changed, request.allow_fallback, result);
    if (result.error != ERROR_SUCCESS) {
        LogLearningError(resolver, request.app_id, L"PublishWrapper", result.error);
    }
    return result;
}
}
