#pragma once
#include "runtime/session_manager.h"
#include <string>
#include <windows.h>


namespace pm::guard {
enum class OriginWaitResult { Released, OriginLost, TimedOut, Failed };
class GuardProcess {
public:
  ~GuardProcess();
  GuardProcess() = default;
  GuardProcess(const GuardProcess &) = delete;
  GuardProcess &operator=(const GuardProcess &) = delete;
  // Copies the SAME PM executable into the active session. Only the PM
  // synchronization handle and readiness event are inherited (STARTUPINFOEX
  // handle allowlist).
  bool Start(const std::wstring &pm_executable,
             pm::runtime::SessionManager &session, DWORD &error);
  HANDLE handle() const noexcept { return process_; }

private:
  HANDLE process_ = nullptr;
};
// Verifies that PM's origin executable, parent directory and volume remain
// accessible without keeping any open file, directory or volume handle on the
// USB drive.
bool IsOriginAccessible(const std::wstring &pm_executable) noexcept;
// Waits with negligible CPU (WM_DEVICECHANGE + RegisterDeviceNotificationW on
// volume interface class + low-frequency passive check) until release_event is
// signaled or the origin executable/volume disappears. Holds no handle on the
// source volume.
OriginWaitResult WaitForReleaseOrOriginLoss(const std::wstring &pm_executable,
                                            HANDLE release_event,
                                            DWORD timeout_ms,
                                            DWORD &error) noexcept;
// Both PM's explicit shutdown and PMG use this path. Only KILL_JOB is enabled
// in phase 3: unsupported future operations fail closed, preserving their
// backups.
bool CleanupSession(pm::runtime::SessionManager &session, DWORD &error);
} // namespace pm::guard
