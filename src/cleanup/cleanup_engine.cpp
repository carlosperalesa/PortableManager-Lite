#include "cleanup/cleanup_module.h"
#include "guard/guard_process.h"
#include "jobs/job_manager.h"
#include "junctions/junction_manager.h"
#include "registry/registry_manager.h"
#include "runtime/session_lock.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace pm::cleanup {
namespace {
using namespace pm::runtime;

struct Pending {
    std::wstring operation;
    std::wstring path;
    DWORD error;
};

std::wstring LogField(std::wstring value) {
    for (auto& c : value) {
        if (c < L' ' || c == 127) c = L' ';
    }
    return value;
}

DWORD WriteLog(const std::vector<Pending>& pending) {
    const auto temp = SessionManager::QuerySystemTempDir();
    if (temp.empty()) return ERROR_PATH_NOT_FOUND;
    const auto path = temp + L"\\pmlog";
    // Serialize writers and refuse unknown files, reparse points and hard links.
    SessionLock lock(L"Local\\PortableManager_pmlog");
    if (!lock.ok()) return lock.error();
    std::wstring lines;
    constexpr std::string_view prefix = "PortableManager ";
    for (const auto& item : pending) {
        lines += L"PortableManager " + item.operation + L"\t" +
                 LogField(item.path) + L"\t" + std::to_wstring(item.error) + L"\r\n";
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, lines.data(),
                                          static_cast<int>(lines.size()), nullptr, 0, nullptr, nullptr);
    if (!size) return ::GetLastError();
    std::string bytes(size, '\0');
    if (!::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, lines.data(),
                               static_cast<int>(lines.size()), bytes.data(), size, nullptr, nullptr))
        return ::GetLastError();

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | FILE_APPEND_DATA, FILE_SHARE_READ,
                                nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const DWORD create_error = file == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
    if (create_error == ERROR_FILE_EXISTS || create_error == ERROR_ALREADY_EXISTS) {
        file = ::CreateFileW(path.c_str(), GENERIC_READ | FILE_APPEND_DATA, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) return ::GetLastError();

    DWORD error = ERROR_SUCCESS;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(file, &info)) {
        error = ::GetLastError();
    } else if (info.nNumberOfLinks != 1 ||
               (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        error = ERROR_INVALID_DATA;
    } else if (create_error != ERROR_SUCCESS) {
        char existing[prefix.size()]{};
        DWORD read = 0;
        if (!::ReadFile(file, existing, sizeof(existing), &read, nullptr)) {
            error = ::GetLastError();
        } else if (read != sizeof(existing) || std::string_view(existing, read) != prefix) {
            error = ERROR_INVALID_DATA;
        }
    }
    if (error == ERROR_SUCCESS) {
        DWORD written = 0;
        if (!::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
            error = ::GetLastError();
        } else if (written != bytes.size()) {
            error = ERROR_WRITE_FAULT;
        }
    }
    ::CloseHandle(file);
    return error;
}

std::wstring Resource(const ManifestOperation& op, const std::wstring& manifest_path) {
    if (op.type == OperationType::RestoreReg) return op.reg_key_path;
    if (op.type == OperationType::RemoveJunction) return op.target_host_path;
    return manifest_path;
}

bool OwnedJob(const ManifestOperation& op, const SessionManager& session) {
    return op.type == OperationType::KillJob && !op.job_name.empty() &&
           op.job_name == jobs::JobManager::MakeName(session.session_id(), op.app_id);
}

void CooperativeClose(const std::vector<ManifestOperation>& operations, DWORD grace) {
    const auto deadline = ::GetTickCount64() + grace;
    std::vector<std::wstring> active;
    for (const auto& op : operations) {
        if (op.type != OperationType::KillJob ||
            op.state == OperationState::IntentRecorded ||
            ManifestManager::IsTerminalAcceptableState(op.state)) continue;
        if (::GetTickCount64() >= deadline) break;
        if (jobs::JobManager::RequestClose(op.job_name, deadline)) active.push_back(op.job_name);
    }
    while (!active.empty()) {
        std::erase_if(active, jobs::JobManager::IsEmpty);
        const auto now = ::GetTickCount64();
        if (now >= deadline) break;
        if (!active.empty()) ::Sleep(static_cast<DWORD>(std::min<ULONGLONG>(10, deadline - now)));
    }
}

Result Execute(SessionManager& session, Mode mode, Artifacts artifacts,
               guard::GuardProcess* guard, bool recovering, bool managed_shadow) {
    const auto fail = [](const std::vector<Pending>& pending) {
        const auto failure = std::find_if(pending.begin(), pending.end(), [](const Pending& item) {
            return item.error != ERROR_IO_PENDING;
        });
        return Result{false, failure != pending.end() ? failure->error : ERROR_IO_PENDING, WriteLog(pending)};
    };
    const auto& path = session.manifest_path();
    if (path.empty()) return {false, ERROR_INVALID_STATE, ERROR_SUCCESS};
    SessionLock lock(session.session_mutex_name());
    if (!lock.ok()) return fail({{L"LOCK_SESSION", path, lock.error()}});

    auto& manifest = session.manifest();
    const bool previously_cleaned = manifest.session_state() == SessionState::Cleaned;
    const auto loaded = manifest.LoadFromDisk();
    if (!loaded.ok) {
        if ((previously_cleaned || recovering) &&
            (loaded.win32_error == ERROR_FILE_NOT_FOUND || loaded.win32_error == ERROR_PATH_NOT_FOUND) &&
            ::GetFileAttributesW(session.session_dir().c_str()) == INVALID_FILE_ATTRIBUTES &&
            (::GetLastError() == ERROR_PATH_NOT_FOUND || ::GetLastError() == ERROR_FILE_NOT_FOUND))
            return {true};
        return fail({{L"LOAD_MANIFEST", path, loaded.win32_error}});
    }

    std::vector<Pending> pending;
    if (manifest.session_state() != SessionState::Cleaned) {
        // Reject a foreign Job or an unsupported inverse before touching resources.
        for (const auto& op : manifest.operations()) {
            if (!OwnedJob(op, session) &&
                !junctions::JunctionManager::IsJunctionOperation(op) &&
                !registry::RegistryManager::IsRegistryOperation(op, session.session_dir()))
                pending.push_back({ManifestManager::OperationTypeToString(op.type),
                                   path, ERROR_NOT_SUPPORTED});
        }
        if (!pending.empty()) return fail(pending);

        bool cooperative = mode != Mode::Critical;
        RollbackHandlers handlers;
        handlers.on_kill_job = [&](const auto& op, DWORD& error) {
            if (cooperative) {
                cooperative = false;
                CooperativeClose(manifest.operations(),
                                 mode == Mode::Fast ? kFastGraceMs : kNormalGraceMs);
            }
            // No process can launch before the Job is durably APPLIED.
            if (op.state == OperationState::IntentRecorded ||
                ManifestManager::IsTerminalAcceptableState(op.state)) return true;
            return jobs::JobManager::TerminateAndWait(op.job_name, error);
        };
        handlers.on_remove_junction = junctions::JunctionManager::Rollback;
        handlers.on_restore_reg = registry::RegistryManager::Rollback;
        handlers.on_release_reg_backup = [&](const auto& op, DWORD& error) {
            if (registry::RegistryManager::ReleaseBackup(op, error)) return true;
            pending.push_back({L"RELEASE_REG_BACKUP", op.backup_host_path, error});
            return false;
        };
        const auto summary = manifest.ExecutePendingRollbacks(handlers);
        if (!summary.all_succeeded) {
            for (const auto& op : manifest.operations()) {
                if (ManifestManager::IsTerminalAcceptableState(op.state)) continue;
                pending.push_back({ManifestManager::OperationTypeToString(op.type),
                                   Resource(op, path),
                                   op.last_error ? op.last_error : ERROR_IO_PENDING});
            }
            if (summary.persistence_error != ERROR_SUCCESS || pending.empty())
                pending.push_back({L"SAVE_MANIFEST", path, summary.persistence_error ?
                                   summary.persistence_error : ERROR_CAN_NOT_COMPLETE});
            return fail(pending);
        }
    }
    DWORD error = ERROR_SUCCESS;
    const auto shadow_path = session.session_dir() + L"\\pm-guard.exe";
    if (guard && !guard->Retire(error)) {
        pending.push_back({L"REMOVE_GUARD_SHADOW", shadow_path, error});
    } else if (managed_shadow && ::GetFileAttributesW(shadow_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // When the owner is gone, Windows still maps the current PMG image.
        pending.push_back({L"REMOVE_GUARD_SHADOW", shadow_path, ERROR_USER_MAPPED_FILE});
    }

    if (!session.FinalizeSessionAndCleanupOwnFiles(artifacts == Artifacts::Remove && pending.empty(), error))
        pending.push_back({L"FINALIZE_SESSION", session.session_dir(), error});
    return pending.empty() ? Result{true} : fail(pending);
}

} // namespace

Result CleanupEngine::Run(SessionManager& session, Mode mode, Artifacts artifacts,
                          guard::GuardProcess* guard) {
    return Execute(session, mode, artifacts, guard, false, false);
}

Result CleanupEngine::Recover(SessionManager& session, bool managed_shadow) {
    return Execute(session, Mode::Critical, managed_shadow ? Artifacts::Remove : Artifacts::KeepManifest,
                   nullptr, true, managed_shadow);
}

} // namespace pm::cleanup
