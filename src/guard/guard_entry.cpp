#include "guard/guard_entry.h"
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
                 std::wstring_view ready_handle_arg) {
    if (!pm::runtime::SessionManager::IsValidSessionId(session_id)) return 2;
    HANDLE parent = ParseHandle(pm_handle_arg);
    HANDLE ready = ParseHandle(ready_handle_arg);
    if (!parent || !ready || parent == ready) return 2;
    const DWORD parent_id = ::GetProcessId(parent);
    if (!parent_id || parent_id == ::GetCurrentProcessId()) return 2;
    pm::runtime::SessionManager session;
    if (!session.OpenExistingSession(session_id).ok) return 3;
    const std::wstring temp = pm::runtime::SessionManager::QuerySystemTempDir();
    if (temp.empty() || !::SetCurrentDirectoryW(temp.c_str())) return 4;
    // Never scan for old sessions. Only open the active session supplied by PM.
    if (!::SetEvent(ready)) return 5;
    ::CloseHandle(ready);
    const DWORD waited = ::WaitForSingleObject(parent, INFINITE);
    ::CloseHandle(parent);
    if (waited != WAIT_OBJECT_0) return 6;
    DWORD error = ERROR_SUCCESS;
    return CleanupSession(session, error) ? 0 : 7;
}
}
