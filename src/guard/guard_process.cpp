#include "guard/guard_process.h"
#include "jobs/job_manager.h"
#include "runtime/session_lock.h"
#include <dbt.h>
#include <vector>

namespace pm::guard {
namespace {
constexpr GUID kVolumeDeviceInterfaceGuid = {
    0x53f5630d,
    0xb6bf,
    0x11d0,
    {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};
constexpr DWORD kOriginPassiveCheckSliceMs = 200;

LRESULT CALLBACK OriginMonitorWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) noexcept {
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND CreateOriginMonitorWindow(HDEVNOTIFY &out_notify) noexcept {
  out_notify = nullptr;
  const HINSTANCE instance = ::GetModuleHandleW(nullptr);
  static const wchar_t kClassName[] = L"PM_OriginMonitorWindowClass";
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = OriginMonitorWndProc;
  wc.hInstance = instance;
  wc.lpszClassName = kClassName;
  ::RegisterClassExW(&wc);

  HWND hwnd = ::CreateWindowExW(0, kClassName, L"", WS_POPUP, 0, 0, 0, 0,
                                nullptr, nullptr, instance, nullptr);
  if (!hwnd)
    return nullptr;

  DEV_BROADCAST_DEVICEINTERFACE_W filter{};
  filter.dbcc_size = sizeof(filter);
  filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  filter.dbcc_classguid = kVolumeDeviceInterfaceGuid;
  out_notify =
      ::RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
  return hwnd;
}
} // namespace

GuardProcess::~GuardProcess() {
  if (process_)
    ::CloseHandle(process_);
}

bool IsOriginAccessible(const std::wstring &pm_executable) noexcept {
  if (pm_executable.empty())
    return false;
  const DWORD exe_attrs = ::GetFileAttributesW(pm_executable.c_str());
  if (exe_attrs == INVALID_FILE_ATTRIBUTES ||
      (exe_attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return false;
  }
  const auto slash = pm_executable.find_last_of(L"\\/");
  if (slash != std::wstring::npos && slash > 0) {
    const std::wstring dir = pm_executable.substr(0, slash);
    const DWORD dir_attrs = ::GetFileAttributesW(dir.c_str());
    if (dir_attrs == INVALID_FILE_ATTRIBUTES ||
        (dir_attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return false;
    }
  }
  if (pm_executable.size() >= 3 && pm_executable[1] == L':' &&
      (pm_executable[2] == L'\\' || pm_executable[2] == L'/')) {
    const wchar_t root[4] = {pm_executable[0], L':', L'\\', L'\0'};
    if (!::GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr,
                                 nullptr, 0)) {
      return false;
    }
  }
  return true;
}

OriginWaitResult WaitForReleaseOrOriginLoss(const std::wstring &pm_executable,
                                            HANDLE release_event,
                                            DWORD timeout_ms,
                                            DWORD &error) noexcept {
  error = ERROR_SUCCESS;
  if (!IsOriginAccessible(pm_executable)) {
    return OriginWaitResult::OriginLost;
  }

  HDEVNOTIFY dev_notify = nullptr;
  HWND hwnd = CreateOriginMonitorWindow(dev_notify);
  const ULONGLONG start_tick = ::GetTickCount64();
  const DWORD handle_count = (release_event != nullptr) ? 1 : 0;
  OriginWaitResult result = OriginWaitResult::TimedOut;

  for (;;) {
    if (!IsOriginAccessible(pm_executable)) {
      result = OriginWaitResult::OriginLost;
      break;
    }

    DWORD wait_slice = kOriginPassiveCheckSliceMs;
    if (timeout_ms != INFINITE) {
      const ULONGLONG elapsed = ::GetTickCount64() - start_tick;
      if (elapsed >= timeout_ms) {
        result = OriginWaitResult::TimedOut;
        break;
      }
      const DWORD remaining = static_cast<DWORD>(timeout_ms - elapsed);
      if (remaining < wait_slice)
        wait_slice = remaining;
    }

    const DWORD waited = ::MsgWaitForMultipleObjects(
        handle_count, release_event ? &release_event : nullptr, FALSE,
        wait_slice, QS_ALLINPUT);

    if (handle_count == 1 && waited == WAIT_OBJECT_0) {
      result = IsOriginAccessible(pm_executable) ? OriginWaitResult::Released
                                                 : OriginWaitResult::OriginLost;
      break;
    }
    if (waited == WAIT_OBJECT_0 + handle_count) {
      MSG msg{};
      while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
      }
      continue;
    }
    if (waited == WAIT_TIMEOUT) {
      continue;
    }
    error = ::GetLastError();
    result = OriginWaitResult::Failed;
    break;
  }

  if (dev_notify)
    ::UnregisterDeviceNotification(dev_notify);
  if (hwnd)
    ::DestroyWindow(hwnd);
  return result;
}
bool GuardProcess::Start(const std::wstring &executable,
                         pm::runtime::SessionManager &session, DWORD &error) {
  error = ERROR_SUCCESS;
  if (process_ || session.session_dir().empty()) {
    error = ERROR_INVALID_STATE;
    return false;
  }
  const std::wstring shadow = session.session_dir() + L"\\pm-guard.exe";
  if (!::CopyFileW(executable.c_str(), shadow.c_str(), TRUE)) {
    error = ::GetLastError();
    return false;
  }
  HANDLE parent = nullptr;
  if (!::DuplicateHandle(
          ::GetCurrentProcess(), ::GetCurrentProcess(), ::GetCurrentProcess(),
          &parent, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) {
    error = ::GetLastError();
    ::DeleteFileW(shadow.c_str());
    return false;
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE ready = ::CreateEventW(&sa, TRUE, FALSE, nullptr);
  if (!ready) {
    error = ::GetLastError();
    ::CloseHandle(parent);
    ::DeleteFileW(shadow.c_str());
    return false;
  }
  SIZE_T bytes = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<unsigned char> buffer(bytes);
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
  const bool initialized = ::InitializeProcThreadAttributeList(
                               startup.lpAttributeList, 1, 0, &bytes) != FALSE;
  HANDLE inherited[] = {parent, ready};
  bool ok = initialized &&
            ::UpdateProcThreadAttribute(
                startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited, sizeof(inherited), nullptr, nullptr);
  PROCESS_INFORMATION pi{};
  if (ok) {
    std::wstring command =
        pm::jobs::QuoteArgument(shadow) + L" --guard " +
        pm::jobs::QuoteArgument(session.session_id()) + L" " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(parent)) + L" " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(ready));
    const std::wstring temp = pm::runtime::SessionManager::QuerySystemTempDir();
    ok = ::CreateProcessW(shadow.c_str(), command.data(), nullptr, nullptr,
                          TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                          nullptr, temp.c_str(), &startup.StartupInfo,
                          &pi) != FALSE;
  }
  if (!ok)
    error = ::GetLastError();
  if (initialized)
    ::DeleteProcThreadAttributeList(startup.lpAttributeList);
  ::CloseHandle(parent);
  if (ok) {
    ::CloseHandle(pi.hThread);
    HANDLE waits[] = {pi.hProcess, ready};
    const DWORD result = ::WaitForMultipleObjects(2, waits, FALSE, 10000);
    ok = result == WAIT_OBJECT_0 + 1;
    if (!ok) {
      error = result == WAIT_TIMEOUT ? WAIT_TIMEOUT : ERROR_PROCESS_ABORTED;
      ::TerminateProcess(pi.hProcess, error);
      ::WaitForSingleObject(pi.hProcess, INFINITE);
      ::CloseHandle(pi.hProcess);
    } else
      process_ = pi.hProcess;
  }
  ::CloseHandle(ready);
  if (!ok)
    ::DeleteFileW(shadow.c_str());
  return ok;
}
bool CleanupSession(pm::runtime::SessionManager &session, DWORD &error) {
  error = ERROR_SUCCESS;
  pm::runtime::SessionLock lock(session.session_mutex_name());
  if (!lock.ok()) {
    error = lock.error();
    return false;
  }
  auto loaded = session.manifest().LoadFromDisk();
  bool ok = loaded.ok;
  if (!ok)
    error = loaded.win32_error;
  if (ok && session.manifest().session_state() !=
                pm::runtime::SessionState::Cleaned) {
    // Validate ALL names and types before invoking any rollback. Do not allow a
    // forged manifest to reference another session's or an unrelated named Job.
    for (const auto &op : session.manifest().operations()) {
      if (op.type != pm::runtime::OperationType::KillJob ||
          op.job_name !=
              pm::jobs::JobManager::MakeName(session.session_id(), op.app_id)) {
        error = ERROR_NOT_SUPPORTED;
        ok = false;
        break;
      }
    }
    if (ok) {
      pm::runtime::RollbackHandlers handlers;
      handlers.on_kill_job = [](const auto &op, DWORD &err) {
        if (pm::runtime::ManifestManager::IsTerminalAcceptableState(op.state)) {
          err = ERROR_SUCCESS;
          return true;
        }
        return pm::jobs::JobManager::TerminateAndWait(op.job_name, err);
      };
      const auto summary = session.manifest().ExecutePendingRollbacks(handlers);
      ok = summary.all_succeeded;
      if (!ok)
        error = ERROR_CAN_NOT_COMPLETE;
    }
  }
  return ok;
}
} // namespace pm::guard
