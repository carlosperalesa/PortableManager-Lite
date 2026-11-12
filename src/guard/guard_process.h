#pragma once
#include "cleanup/cleanup_module.h"
#include <functional>
#include <string>
#include <windows.h>


namespace pm::guard {
inline constexpr wchar_t kShadowFilename[] = L"pml.exe";
enum class OriginWaitResult { Released, Stopped, OriginLost, TimedOut, Failed };
class GuardProcess {
public:
  ~GuardProcess();
  GuardProcess() = default;
  GuardProcess(const GuardProcess &) = delete;
  GuardProcess &operator=(const GuardProcess &) = delete;
  // Copies the SAME PM executable into the active session. Only the PM
  // synchronization handle, readiness/retirement events and an optional resource
  // reservation are inherited through the STARTUPINFOEX allowlist.
  bool Start(const std::wstring &pm_executable,
             pm::runtime::SessionManager &session, DWORD &error,
             HANDLE resource_reservation = nullptr,
             cleanup::Artifacts artifacts = cleanup::Artifacts::Remove,
             HANDLE app_reservation = nullptr,
             const std::function<bool(HANDLE)>& before_resume = {});
  HANDLE handle() const noexcept { return process_; }
  // Only after durable restoration. Stop the idle guard and remove its own image.
  bool Retire(DWORD& error);

private:
  HANDLE stop_ = nullptr;
  HANDLE shadow_ = nullptr;
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
                                            DWORD &error,
                                            HANDLE stop_event = nullptr) noexcept;
// Both PM's explicit shutdown and PMG use this path. Controlled Jobs stop
// before any managed junction is removed or backup restored.
bool CleanupSession(pm::runtime::SessionManager &session, DWORD &error);
} // namespace pm::guard
