#include "applications/session_host.h"

namespace pm::applications {

DWORD SessionHost::Start(std::wstring origin, std::function<void()> notify) {
    origin_ = std::move(origin);
    const auto init = session_.InitializeNewSession();
    if (!init.ok) return init.win32_error;
    const auto active = session_.TransitionState(runtime::SessionState::Active);
    if (!active.ok) return active.win32_error;
    guard_.Start(origin_, session_, std::move(notify));
    return ERROR_SUCCESS;
}

DWORD SessionHost::Add(const std::wstring& app, std::shared_ptr<jobs::JobManager> job,
                       std::span<const HANDLE> reservations, bool require_guard) {
    if (closing()) return ERROR_CANCELLED;
    auto [entry, inserted] = jobs_.try_emplace(app, std::move(job));
    if (!inserted) return ERROR_ALREADY_EXISTS;
    const DWORD error = guard_.Protect(app, reservations, require_guard);
    if (error) jobs_.erase(entry);
    return error;
}

void SessionHost::SetPreRollback(const std::wstring& app, std::function<DWORD()> fn) {
    pre_rollback_[app] = std::move(fn);
}

void SessionHost::Release(const std::wstring& app) {
    jobs_.erase(app);
    pre_rollback_.erase(app);
    guard_.Release(app);
}

DWORD SessionHost::Close(cleanup::Mode mode) {
    BeginShutdown();
    guard_.StopMonitoring();
    if (session_.manifest_path().empty()) return ERROR_SUCCESS;
    DWORD pre_err = ERROR_SUCCESS;
    if (mode != cleanup::Mode::Critical && !pre_rollback_.empty()) {
        for (const auto& [id, job] : jobs_) {
            if (!job) continue;
            DWORD jerr = ERROR_SUCCESS;
            jobs::JobManager::TerminateAndWait(job->name(), jerr);
        }
        for (const auto& [id, fn] : pre_rollback_) {
            if (!fn) continue;
            const DWORD err = fn();
            if (err && !pre_err) pre_err = err;
        }
    }
    const DWORD clean_err =
        cleanup::CleanupEngine::Run(session_, mode, cleanup::Artifacts::Remove, &guard_.process()).error;
    return clean_err ? clean_err : pre_err;
}

} // namespace pm::applications
