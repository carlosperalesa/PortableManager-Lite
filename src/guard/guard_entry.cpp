#include "guard/guard_entry.h"
#include "cleanup/cleanup_module.h"
#include "guard/guard_process.h"
#include "runtime/session_manager.h"
#include <windows.h>
#include <limits>

namespace pm::guard {
namespace {
HANDLE ParseHandle(std::wstring_view text) {
    if (text.empty()) return nullptr;
    ULONG_PTR number = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9' || number > (std::numeric_limits<ULONG_PTR>::max() - (c - L'0')) / 10)
            return nullptr;
        number = number * 10 + (c - L'0');
    }
    return reinterpret_cast<HANDLE>(number);
}
}
int RunGuardMode(std::wstring_view session_id, std::wstring_view pm_handle_arg,
                 std::wstring_view ready_handle_arg, std::wstring_view stop_handle_arg) {
    if (!pm::runtime::SessionManager::IsValidSessionId(session_id)) return 2;
    HANDLE parent = ParseHandle(pm_handle_arg);
    HANDLE ready = ParseHandle(ready_handle_arg);
    HANDLE stop = ParseHandle(stop_handle_arg);
    if (!parent || !ready || parent == ready ||
        (!stop_handle_arg.empty() && (!stop || stop == parent || stop == ready))) return 2;
    const DWORD parent_id = ::GetProcessId(parent);
    if (!parent_id || parent_id == ::GetCurrentProcessId()) return 2;
    pm::runtime::SessionManager session;
    if (!session.OpenExistingSession(session_id).ok) return 3;
    const std::wstring temp = pm::runtime::SessionManager::QuerySystemTempDir();
    if (temp.empty() || !::SetCurrentDirectoryW(temp.c_str())) return 4;
    // The normal retirement event cannot start a cleanup. While PM is
    // running, PMG either stays blocked or exits after PM has restored the host.
    if (!::SetEvent(ready)) return 5;
    ::CloseHandle(ready);

    wchar_t origin_buf[32768]{};
    DWORD origin_len = static_cast<DWORD>(std::size(origin_buf));
    const bool has_origin = ::QueryFullProcessImageNameW(parent, 0, origin_buf, &origin_len) != FALSE && origin_len > 0;

    bool should_recover = false;
    bool clean_stop = false;
    bool wait_failed = false;

    if (has_origin) {
        DWORD wait_err = ERROR_SUCCESS;
        const auto res = WaitForReleaseOrOriginLoss(
            std::wstring(origin_buf, origin_len), parent, INFINITE, wait_err, stop);
        if (res == OriginWaitResult::Stopped) {
            clean_stop = true;
        } else if (res == OriginWaitResult::Released || res == OriginWaitResult::OriginLost) {
            should_recover = true;
        } else {
            wait_failed = true;
        }
    } else {
        const HANDLE waits[]{parent, stop};
        const DWORD waited = ::WaitForMultipleObjects(stop ? 2 : 1, waits, FALSE, INFINITE);
        if (stop && waited == WAIT_OBJECT_0 + 1) clean_stop = true;
        else if (waited == WAIT_OBJECT_0) should_recover = true;
        else wait_failed = true;
    }

    ::CloseHandle(parent);
    if (stop) ::CloseHandle(stop);
    if (clean_stop) return 0;
    if (wait_failed || !should_recover) return 6;
    return cleanup::CleanupEngine::Recover(session, stop != nullptr).ok ? 0 : 7;
}
}
