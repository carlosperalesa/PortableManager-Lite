#include "guard/guard_process.h"
#include "jobs/job_manager.h"
#include "runtime/session_lock.h"
#include <vector>

namespace pm::guard {
GuardProcess::~GuardProcess() { if (process_) ::CloseHandle(process_); }
bool GuardProcess::Start(const std::wstring& executable, pm::runtime::SessionManager& session, DWORD& error) {
    error = ERROR_SUCCESS;
    if (process_ || session.session_dir().empty()) { error = ERROR_INVALID_STATE; return false; }
    const std::wstring shadow = session.session_dir() + L"\\pm-guard.exe";
    if (!::CopyFileW(executable.c_str(), shadow.c_str(), TRUE)) { error = ::GetLastError(); return false; }
    HANDLE parent = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(), ::GetCurrentProcess(),
                           &parent, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) {
        error = ::GetLastError(); ::DeleteFileW(shadow.c_str()); return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE ready = ::CreateEventW(&sa, TRUE, FALSE, nullptr);
    if (!ready) { error = ::GetLastError(); ::CloseHandle(parent); ::DeleteFileW(shadow.c_str()); return false; }
    SIZE_T bytes = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    const bool initialized = ::InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes) != FALSE;
    HANDLE inherited[] = {parent, ready};
    bool ok = initialized && ::UpdateProcThreadAttribute(startup.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
    PROCESS_INFORMATION pi{};
    if (ok) {
        std::wstring command = pm::jobs::QuoteArgument(shadow) + L" --guard " +
            pm::jobs::QuoteArgument(session.session_id()) + L" " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(parent)) + L" " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(ready));
        const std::wstring temp = pm::runtime::SessionManager::QuerySystemTempDir();
        ok = ::CreateProcessW(shadow.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, temp.c_str(), &startup.StartupInfo, &pi) != FALSE;
    }
    if (!ok) error = ::GetLastError();
    if (initialized) ::DeleteProcThreadAttributeList(startup.lpAttributeList);
    ::CloseHandle(parent);
    if (ok) {
        ::CloseHandle(pi.hThread);
        HANDLE waits[] = {pi.hProcess, ready};
        const DWORD result = ::WaitForMultipleObjects(2, waits, FALSE, 10000);
        ok = result == WAIT_OBJECT_0 + 1;
        if (!ok) {
            error = result == WAIT_TIMEOUT ? WAIT_TIMEOUT : ERROR_PROCESS_ABORTED;
            ::TerminateProcess(pi.hProcess, error); ::WaitForSingleObject(pi.hProcess, INFINITE);
            ::CloseHandle(pi.hProcess);
        } else process_ = pi.hProcess;
    }
    ::CloseHandle(ready);
    if (!ok) ::DeleteFileW(shadow.c_str());
    return ok;
}
bool CleanupSession(pm::runtime::SessionManager& session, DWORD& error) {
    error = ERROR_SUCCESS;
    pm::runtime::SessionLock lock(session.session_mutex_name());
    if (!lock.ok()) { error = lock.error(); return false; }
    auto loaded = session.manifest().LoadFromDisk();
    bool ok = loaded.ok;
    if (!ok) error = loaded.win32_error;
    if (ok && session.manifest().session_state() != pm::runtime::SessionState::Cleaned) {
        // Validate ALL names and types before invoking any rollback. Do not allow a
        // forged manifest to reference another session's or an unrelated named Job.
        for (const auto& op : session.manifest().operations()) {
            if (op.type != pm::runtime::OperationType::KillJob ||
                op.job_name != pm::jobs::JobManager::MakeName(session.session_id(), op.app_id)) {
                error = ERROR_NOT_SUPPORTED; ok = false; break;
            }
        }
        if (ok) {
            pm::runtime::RollbackHandlers handlers;
            handlers.on_kill_job = [](const auto& op, DWORD& err) {
                if (pm::runtime::ManifestManager::IsTerminalAcceptableState(op.state)) {
                    err = ERROR_SUCCESS; return true;
                }
                return pm::jobs::JobManager::TerminateAndWait(op.job_name, err);
            };
            const auto summary = session.manifest().ExecutePendingRollbacks(handlers);
            ok = summary.all_succeeded;
            if (!ok) error = ERROR_CAN_NOT_COMPLETE;
        }
    }
    return ok;
}
}
