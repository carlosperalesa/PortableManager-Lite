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
                       std::span<const HANDLE> reservations) {
    if (closing()) return ERROR_CANCELLED;
    auto [entry, inserted] = jobs_.try_emplace(app, std::move(job));
    if (!inserted) return ERROR_ALREADY_EXISTS;
    const DWORD error = guard_.Protect(app, reservations);
    if (error) jobs_.erase(entry);
    return error;
}

void SessionHost::Release(const std::wstring& app) {
    jobs_.erase(app);
    guard_.Release(app);
}

DWORD SessionHost::Close(cleanup::Mode mode) {
    BeginShutdown();
    guard_.StopMonitoring();
    if (session_.manifest_path().empty()) return ERROR_SUCCESS;
    return cleanup::CleanupEngine::Run(session_, mode, cleanup::Artifacts::Remove, &guard_.process()).error;
}

} // namespace pm::applications
