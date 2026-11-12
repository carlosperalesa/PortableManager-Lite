#include "guard/guard_process.h"
#include "jobs/job_manager.h"
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
  if (process_) ::CloseHandle(process_);
  if (stop_) ::CloseHandle(stop_);
  if (shadow_) ::CloseHandle(shadow_);
}

bool GuardProcess::Retire(DWORD& error) {
  error = ERROR_SUCCESS;
  if (!process_ || !stop_ || !shadow_) return true;
  if (!::SetEvent(stop_)) {
    error = ::GetLastError();
    return false;
  }
  const DWORD waited = ::WaitForSingleObject(process_, 10000);
  if (waited != WAIT_OBJECT_0) {
    error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : ::GetLastError();
    return false;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!::GetFileInformationByHandle(shadow_, &info)) {
    error = ::GetLastError();
    return false;
  }
  if (info.nNumberOfLinks != 1 ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
    error = ERROR_INVALID_DATA;
    return false;
  }
  FILE_DISPOSITION_INFO remove{TRUE};
  if (!::SetFileInformationByHandle(shadow_, FileDispositionInfo, &remove, sizeof(remove))) {
    error = ::GetLastError();
    return false;
  }
  ::CloseHandle(shadow_);
  shadow_ = nullptr;
  return true;
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
                         pm::runtime::SessionManager &session, DWORD &error,
                         HANDLE resource_reservation, cleanup::Artifacts artifacts) {
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
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  const bool retire = artifacts == cleanup::Artifacts::Remove;
  HANDLE parent = nullptr;
  HANDLE ready = nullptr;
  HANDLE stop = nullptr;
  HANDLE shadow_file = INVALID_HANDLE_VALUE;
  const auto release = [&] {
    if (parent) ::CloseHandle(parent);
    if (ready) ::CloseHandle(ready);
    if (stop) ::CloseHandle(stop);
    if (shadow_file != INVALID_HANDLE_VALUE) ::CloseHandle(shadow_file);
  };
  bool ok = ::DuplicateHandle(
      ::GetCurrentProcess(), ::GetCurrentProcess(), ::GetCurrentProcess(),
      &parent, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0) != FALSE;
  if (ok) {
    ready = ::CreateEventW(&sa, TRUE, FALSE, nullptr);
    ok = ready != nullptr;
  }
  if (ok && retire) {
    stop = ::CreateEventW(&sa, TRUE, FALSE, nullptr);
    ok = stop != nullptr;
  }
  if (ok && retire) {
    // Hold the actual copy through retirement, so no later name lookup deletes
    // a different file. The image loader needs delete sharing.
    shadow_file = ::CreateFileW(shadow.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    ok = shadow_file != INVALID_HANDLE_VALUE;
  }
  if (!ok) {
    error = ::GetLastError();
    release();
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
  // The caller owns the original reservation. A duplicate keeps the named
  // object alive in PMG even if PM dies before restoring its resources.
  HANDLE reservation = nullptr;
  const bool reserved = !resource_reservation ||
      ::DuplicateHandle(::GetCurrentProcess(), resource_reservation, ::GetCurrentProcess(),
                        &reservation, 0, TRUE, DUPLICATE_SAME_ACCESS);
  HANDLE inherited[] = {parent, ready, stop, reservation};
  SIZE_T inherited_count = 2;
  if (stop) inherited[inherited_count++] = stop;
  if (reservation) inherited[inherited_count++] = reservation;
  ok = reserved && initialized &&
            ::UpdateProcThreadAttribute(
                startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited, inherited_count * sizeof(HANDLE), nullptr, nullptr);
  PROCESS_INFORMATION pi{};
  if (ok) {
    std::wstring command =
        pm::jobs::QuoteArgument(shadow) + L" --guard " +
        pm::jobs::QuoteArgument(session.session_id()) + L" " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(parent)) + L" " +
        std::to_wstring(reinterpret_cast<ULONG_PTR>(ready));
    if (stop)
      command += L" " + std::to_wstring(reinterpret_cast<ULONG_PTR>(stop));
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
  if (reservation)
    ::CloseHandle(reservation);
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
    } else {
      process_ = pi.hProcess;
      stop_ = stop;
      shadow_ = retire ? shadow_file : nullptr;
      stop = nullptr;
      shadow_file = INVALID_HANDLE_VALUE;
    }
  }
  release();
  if (!ok)
    ::DeleteFileW(shadow.c_str());
  return ok;
}
bool CleanupSession(pm::runtime::SessionManager &session, DWORD &error) {
  const auto result = cleanup::CleanupEngine::Run(
      session, cleanup::Mode::Critical, cleanup::Artifacts::KeepManifest);
  error = result.error;
  return result.ok;
}
} // namespace pm::guard
