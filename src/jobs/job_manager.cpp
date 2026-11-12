#include "jobs/job_manager.h"
#include "runtime/session_lock.h"

namespace pm::jobs {
std::wstring QuoteArgument(std::wstring_view value) {
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"') out += L'\\';
        out += c;
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
JobManager::~JobManager() { if (job_) ::CloseHandle(job_); }
std::wstring JobManager::MakeName(std::wstring_view session, std::wstring_view instance) {
    if (!pm::runtime::SessionManager::IsValidSessionId(session) ||
        !pm::runtime::SessionManager::IsValidSessionId(instance)) return {};
    return L"Local\\PM_Job_" + std::to_wstring(session.size()) + L"_" + std::wstring(session) + L"_" + std::wstring(instance);
}
bool JobManager::Create(pm::runtime::SessionManager& session, std::wstring_view instance, DWORD& error) {
    error = ERROR_SUCCESS;
    if (job_) { error = ERROR_INVALID_STATE; return false; }
    name_ = MakeName(session.session_id(), instance);
    if (name_.empty()) { error = ERROR_INVALID_PARAMETER; return false; }
    pm::runtime::SessionLock lock(session.session_mutex_name());
    if (!lock.ok()) { error = lock.error(); return false; }
    const auto loaded = session.manifest().LoadFromDisk();
    if (!loaded.ok) { error = loaded.win32_error; return false; }
    const auto state = session.manifest().session_state();
    if (state != pm::runtime::SessionState::Preparing && state != pm::runtime::SessionState::Active) {
        error = ERROR_INVALID_STATE; return false;
    }
    // Reserve an EMPTY named Job exclusively. A collision must never create a
    // KILL_JOB record for an object owned by somebody else.
    ::SetLastError(ERROR_SUCCESS);
    job_ = ::CreateJobObjectW(nullptr, name_.c_str());
    error = ::GetLastError();
    if (!job_) return false;
    if (error == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(job_); job_ = nullptr;
        return false;
    }
    // Write-ahead before setting limits, assigning or running any process.
    pm::runtime::ManifestOperation op;
    op.type = pm::runtime::OperationType::KillJob;
    op.app_id = instance;
    op.job_name = name_;
    std::wstring id;
    const auto intent = session.manifest().RecordOperationIntent(op, id);
    if (!intent.ok) {
        error = intent.win32_error; ::CloseHandle(job_); job_ = nullptr; return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = ::GetLastError(); ::CloseHandle(job_); job_ = nullptr; return false;
    }
    const auto applied = session.manifest().UpdateOperationState(id, pm::runtime::OperationState::Applied);
    error = applied.win32_error;
    if (applied.ok) {
        session_id_ = session.session_id(); manifest_path_ = session.manifest_path();
        mutex_name_ = session.session_mutex_name(); operation_id_ = id;
    }
    if (!applied.ok) { ::CloseHandle(job_); job_ = nullptr; }
    return applied.ok;
}
bool JobManager::Launch(const std::wstring& executable, std::wstring_view arguments,
                        const std::wstring& cwd, PROCESS_INFORMATION& process, DWORD& error, ConsoleMode console) {
    process = {};
    if (!job_) { error = ERROR_INVALID_HANDLE; return false; }
    pm::runtime::SessionLock lock(mutex_name_);
    if (!lock.ok()) { error = lock.error(); return false; }
    pm::runtime::ManifestManager current(session_id_, manifest_path_);
    const auto loaded = current.LoadFromDisk();
    if (!loaded.ok) { error = loaded.win32_error; return false; }
    const auto* operation = current.FindOperation(operation_id_);
    if ((current.session_state() != pm::runtime::SessionState::Preparing &&
         current.session_state() != pm::runtime::SessionState::Active) || !operation ||
        operation->state != pm::runtime::OperationState::Applied || operation->job_name != name_) {
        error = ERROR_INVALID_STATE; return false;
    }
    std::wstring cmd = QuoteArgument(executable);
    if (!arguments.empty()) cmd += L" " + std::wstring(arguments);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    if (!::CreateProcessW(executable.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                          CREATE_SUSPENDED | (console == ConsoleMode::Detached ? DETACHED_PROCESS : CREATE_NO_WINDOW), nullptr,
                          cwd.empty() ? nullptr : cwd.c_str(), &startup, &process)) {
        error = ::GetLastError(); return false;
    }
    if (!::AssignProcessToJobObject(job_, process.hProcess) ||
        ::ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        error = ::GetLastError();
        ::TerminateProcess(process.hProcess, error);
        ::WaitForSingleObject(process.hProcess, INFINITE);
        ::CloseHandle(process.hThread); ::CloseHandle(process.hProcess); process = {};
        return false;
    }
    error = ERROR_SUCCESS; return true;
}
bool JobManager::TerminateAndWait(const std::wstring& name, DWORD& error) {
    error = ERROR_SUCCESS;
    HANDLE job = ::OpenJobObjectW(JOB_OBJECT_TERMINATE | JOB_OBJECT_QUERY, FALSE, name.c_str());
    if (!job) {
        error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) { error = ERROR_SUCCESS; return true; }
        return false;
    }
    bool ok = ::TerminateJobObject(job, ERROR_PROCESS_ABORTED) != FALSE;
    if (!ok) error = ::GetLastError();
    const ULONGLONG deadline = ::GetTickCount64() + 10000;
    while (ok) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        if (!::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr)) {
            error = ::GetLastError(); ok = false; break;
        }
        if (info.ActiveProcesses == 0) break;
        if (::GetTickCount64() >= deadline) { error = WAIT_TIMEOUT; ok = false; break; }
        ::Sleep(10); // Bounded drain of terminated Job; never used to monitor PM.
    }
    ::CloseHandle(job);
    return ok;
}
}
