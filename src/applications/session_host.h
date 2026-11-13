#pragma once

#include "guard/guard_supervisor.h"
#include "jobs/job_manager.h"

#include <functional>
#include <map>
#include <memory>

namespace pm::applications {

// Lifetime of a running PML instance, including when its app list is empty.
class SessionHost {
public:
    DWORD Start(std::wstring origin, std::function<void()> notify = {});
    void BeginShutdown() noexcept { guard_.BeginShutdown(); closing_.store(true); }
    bool closing() const noexcept { return closing_.load(); }
    bool protected_by_guard() const noexcept { return guard_.ready(); }
    const std::wstring& origin() const noexcept { return origin_; }
    runtime::SessionManager& session() noexcept { return session_; }

    // Called under the named session mutex from preparation and unit cleanup.
    DWORD Add(const std::wstring& app, std::shared_ptr<jobs::JobManager> job, std::span<const HANDLE> reservations,
              bool require_guard);
    void SetPreRollback(const std::wstring& app, std::function<DWORD()> fn);
    void Release(const std::wstring& app);
    DWORD Close(cleanup::Mode mode);

private:
    std::wstring origin_;
    runtime::SessionManager session_;
    guard::GuardSupervisor guard_;
    std::map<std::wstring, std::shared_ptr<jobs::JobManager>> jobs_;
    std::map<std::wstring, std::function<DWORD()>> pre_rollback_;
    std::atomic<bool> closing_{false};
};

} // namespace pm::applications
