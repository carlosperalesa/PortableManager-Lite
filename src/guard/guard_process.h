#pragma once
#include "runtime/session_manager.h"
#include <windows.h>
#include <string>

namespace pm::guard {
class GuardProcess {
public:
    ~GuardProcess();
    GuardProcess() = default;
    GuardProcess(const GuardProcess&) = delete;
    GuardProcess& operator=(const GuardProcess&) = delete;
    // Copies the SAME PM executable into the active session. Only the PM synchronization
    // handle and readiness event are inherited (STARTUPINFOEX handle allowlist).
    bool Start(const std::wstring& pm_executable, pm::runtime::SessionManager& session, DWORD& error);
    HANDLE handle() const noexcept { return process_; }
private:
    HANDLE process_ = nullptr;
};
// Both PM's explicit shutdown and PMG use this path. Only KILL_JOB is enabled in
// phase 3: unsupported future operations fail closed, preserving their backups.
bool CleanupSession(pm::runtime::SessionManager& session, DWORD& error);
}
