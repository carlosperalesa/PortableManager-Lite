#include "guard/guard_process.h"
#include "jobs/job_manager.h"
#include "core/atomic_file.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using pm::runtime::SessionManager;
using pm::runtime::SessionState;
std::wstring self;
std::wstring Q(std::wstring_view s) { return pm::jobs::QuoteArgument(s); }
void Close(PROCESS_INFORMATION& p) {
    if (p.hThread) ::CloseHandle(p.hThread);
    if (p.hProcess) ::CloseHandle(p.hProcess);
    p = {};
}
bool Spawn(const std::wstring& exe, const std::wstring& args, PROCESS_INFORMATION& p) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    std::wstring command = Q(exe) + L" " + args;
    const std::wstring temp = SessionManager::QuerySystemTempDir();
    return ::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, temp.c_str(), &si, &p) != FALSE;
}
bool WaitExit(HANDLE process, DWORD expected = 0) {
    DWORD code = 999;
    return ::WaitForSingleObject(process, 15000) == WAIT_OBJECT_0 &&
        ::GetExitCodeProcess(process, &code) && code == expected;
}
ULONGLONG Ticks(FILETIME ft) { return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }
ULONGLONG Cpu(HANDLE process) {
    FILETIME c{}, e{}, k{}, u{};
    if (!::GetProcessTimes(process, &c, &e, &k, &u)) return ~0ULL;
    return Ticks(k) + Ticks(u);
}
HANDLE Event(const std::wstring& id, const wchar_t* suffix) {
    return ::CreateEventW(nullptr, TRUE, FALSE, (L"Local\\PM_Test_" + id + suffix).c_str());
}
std::wstring SessionPath(const std::wstring& id) {
    return SessionManager::QuerySystemTempDir() + L"\\PortableManager\\Session\\" + id;
}
// Controller is a real PM-owner process. Its guard is the actual pm.exe shadow;
// its workers are this synthetic helper, never a user application.
int Controller(int argc, wchar_t** argv) {
    if (argc != 5) return 2;
    const std::wstring id = argv[2], pm = argv[3], mode = argv[4];
    HANDLE ready = Event(id, L"_ready"), release = Event(id, L"_release");
    SessionManager session;
    DWORD error = 0;
    if (!session.InitializeSessionWithId(id).ok) return 3;
    pm::jobs::JobManager job;
    if (!job.Create(session, L"synthetic", error)) return 4;
    pm::guard::GuardProcess guard;
    if (!guard.Start(pm, session, error)) return 5;
    PROCESS_INFORMATION worker{};
    if (!job.Launch(self, L"--tree " + Q(id), SessionManager::QuerySystemTempDir(), worker, error)) return 6;
    HANDLE tree_ready = Event(id, L"_tree");
    if (::WaitForSingleObject(tree_ready, 10000) != WAIT_OBJECT_0) return 7;
    ::CloseHandle(tree_ready);
    if (!session.TransitionState(SessionState::Active).ok) return 8;
    const std::string info = std::to_string(::GetProcessId(guard.handle())) + " " + std::to_string(worker.dwProcessId);
    if (!pm::core::AtomicFile::WriteUtf8Text(session.session_dir() + L"\\test-info.txt", info, false).ok) return 9;
    ::SetEvent(ready);
    ::WaitForSingleObject(release, INFINITE);
    if (mode == L"normal" || mode == L"guard-dead" || mode == L"children-dead") {
        // Calling twice must perform no second rollback after CLEANED.
        if (!pm::guard::CleanupSession(session, error) || !pm::guard::CleanupSession(session, error)) return 10;
    } else {
        ::TerminateProcess(::GetCurrentProcess(), 77); // No destructors, abrupt PM disappearance.
    }
    Close(worker); ::CloseHandle(ready); ::CloseHandle(release);
    return 0;
}
int Tree(const std::wstring& id) {
    PROCESS_INFORMATION leaf{};
    if (!Spawn(self, L"--leaf", leaf)) return 2;
    HANDLE ready = Event(id, L"_tree");
    ::SetEvent(ready); ::CloseHandle(ready);
    Close(leaf); ::Sleep(INFINITE); return 0;
}
// Only the exact session created by this test is removed, after all known handles
// signal exit. Keep failed sessions and evidence for diagnosis.
void RemoveSuccessfulSession(const std::wstring& id) {
    const std::wstring dir = SessionPath(id);
    ::DeleteFileW((dir + L"\\pm-guard.exe").c_str());
    ::DeleteFileW((dir + L"\\test-info.txt").c_str());
    ::DeleteFileW((dir + L"\\session.ini").c_str());
    ::RemoveDirectoryW(dir.c_str());
}
bool Lifecycle(const std::wstring& pm, const std::wstring& mode, bool remove_source = false) {
    const std::wstring id = SessionManager::GenerateSessionGuid();
    HANDLE ready = Event(id, L"_ready"), release = Event(id, L"_release");
    PROCESS_INFORMATION owner{}, outsider{};
    HANDLE guard = nullptr, worker = nullptr, job = nullptr;
    bool ok = Spawn(self, L"--leaf", outsider) && Spawn(self, L"--controller " + Q(id) + L" " + Q(pm) + L" " + Q(mode), owner);
    if (ok) ok = ::WaitForSingleObject(ready, 15000) == WAIT_OBJECT_0;
    if (ok) {
        auto info = pm::core::AtomicFile::ReadUtf8Text(SessionPath(id) + L"\\test-info.txt");
        unsigned long guard_id = 0, worker_id = 0;
        ok = info.ok && std::sscanf(info.content.c_str(), "%lu %lu", &guard_id, &worker_id) == 2;
        if (ok) {
            guard = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, guard_id);
            worker = ::OpenProcess(SYNCHRONIZE, FALSE, worker_id);
            job = ::OpenJobObjectW(JOB_OBJECT_QUERY | JOB_OBJECT_TERMINATE, FALSE,
                pm::jobs::JobManager::MakeName(id, L"synthetic").c_str());
            ok = guard && worker && job;
        }
    }
    if (ok) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        ok = ::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) &&
            accounting.ActiveProcesses == 2; // Launcher + its descendant both assigned.
        const ULONGLONG before = Cpu(guard);
        ::Sleep(1200); // Measurement interval in test only.
        const ULONGLONG after = Cpu(guard);
        std::wprintf(L"[CPU] mode=%ls delta_100ns=%llu interval_ms=1200\n", mode.c_str(), after - before);
        SessionManager observed;
        ok = ok && before != ~0ULL && after >= before && (after - before) <= 200000 &&
            ::WaitForSingleObject(guard, 0) == WAIT_TIMEOUT &&
            ::WaitForSingleObject(worker, 0) == WAIT_TIMEOUT &&
            observed.OpenExistingSession(id).ok && observed.manifest().session_state() == SessionState::Active;
        if (remove_source) {
            const std::wstring source_dir = pm.substr(0, pm.find_last_of(L"\\/"));
            ok = ok && ::DeleteFileW(pm.c_str()) && ::RemoveDirectoryW(source_dir.c_str());
        }
        if (mode == L"guard-dead") {
            ok = ok && ::TerminateProcess(guard, 88) && WaitExit(guard, 88);
        }
        if (mode == L"children-dead") {
            ok = ok && ::TerminateJobObject(job, 89) &&
                ::WaitForSingleObject(worker, 10000) == WAIT_OBJECT_0 &&
                ::WaitForSingleObject(owner.hProcess, 0) == WAIT_TIMEOUT &&
                ::WaitForSingleObject(guard, 0) == WAIT_TIMEOUT;
        }
    }
    ::SetEvent(release);
    if (ok) ok = WaitExit(owner.hProcess, mode == L"crash" ? 77 : 0);
    if (ok && mode != L"guard-dead") ok = WaitExit(guard);
    if (ok) {
        SessionManager observed;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        ok = ::WaitForSingleObject(worker, 10000) == WAIT_OBJECT_0 &&
            ::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) &&
            accounting.ActiveProcesses == 0 && observed.OpenExistingSession(id).ok &&
            observed.manifest().session_state() == SessionState::Cleaned &&
            ::WaitForSingleObject(outsider.hProcess, 0) == WAIT_TIMEOUT;
    }
    // Failure containment applies exclusively to helpers created by this test.
    if (owner.hProcess && ::WaitForSingleObject(owner.hProcess, 0) == WAIT_TIMEOUT) {
        ::TerminateProcess(owner.hProcess, 99); ::WaitForSingleObject(owner.hProcess, 10000);
    }
    if (guard && ::WaitForSingleObject(guard, 15000) == WAIT_TIMEOUT) {
        ::TerminateProcess(guard, 99); ::WaitForSingleObject(guard, 10000);
    }
    if (job) { ::TerminateJobObject(job, 99); ::CloseHandle(job); }
    if (outsider.hProcess) { ::TerminateProcess(outsider.hProcess, 99); ::WaitForSingleObject(outsider.hProcess, 10000); }
    if (guard) ::CloseHandle(guard);
    if (worker) ::CloseHandle(worker);
    Close(owner); Close(outsider); ::CloseHandle(ready); ::CloseHandle(release);
    if (ok) RemoveSuccessfulSession(id);
    return ok;
}
bool MissingSource(const std::wstring& pm) {
    const std::wstring dir = SessionManager::QuerySystemTempDir() + L"\\PM_Source_" + SessionManager::GenerateSessionGuid();
    if (!::CreateDirectoryW(dir.c_str(), nullptr)) return false;
    const std::wstring copy = dir + L"\\pm.exe";
    if (!::CopyFileW(pm.c_str(), copy.c_str(), TRUE)) return false;
    // Remove the original while PM is alive, then abruptly terminate PM. The
    // independently copied guard must finish without accessing the source.
    const bool ok = Lifecycle(copy, L"crash", true);
    ::DeleteFileW(copy.c_str()); ::RemoveDirectoryW(dir.c_str());
    return ok;
}
bool KillOnLastClose() {
    SessionManager session;
    const auto init = session.InitializeNewSession();
    if (!init.ok) return false;
    DWORD error = 0;
    PROCESS_INFORMATION p{};
    bool ok = false;
    {
        pm::jobs::JobManager job;
        ok = job.Create(session, L"lastclose", error) &&
            job.Launch(self, L"--leaf", SessionManager::QuerySystemTempDir(), p, error);
    }
    if (ok) ok = ::WaitForSingleObject(p.hProcess, 10000) == WAIT_OBJECT_0 &&
        pm::guard::CleanupSession(session, error);
    if (p.hProcess && ::WaitForSingleObject(p.hProcess, 0) == WAIT_TIMEOUT) ::TerminateProcess(p.hProcess, 99);
    Close(p);
    if (ok) RemoveSuccessfulSession(init.session_id);
    return ok;
}
bool RejectLaunchAfterCleanup() {
    SessionManager session;
    const auto init = session.InitializeNewSession();
    if (!init.ok) return false;
    DWORD error = 0;
    pm::jobs::JobManager job;
    PROCESS_INFORMATION p{};
    bool ok = job.Create(session, L"closed", error) && pm::guard::CleanupSession(session, error) &&
        !job.Launch(self, L"--leaf", L"", p, error) && error == ERROR_INVALID_STATE;
    if (p.hProcess) { ::TerminateProcess(p.hProcess, 99); Close(p); }
    if (ok) RemoveSuccessfulSession(init.session_id);
    return ok;
}

bool FailureBarrier() {
    SessionManager session;
    const auto init = session.InitializeNewSession();
    if (!init.ok) return false;
    pm::runtime::ManifestOperation dir;
    dir.type = pm::runtime::OperationType::DeleteDir;
    dir.target_host_path = session.session_dir() + L"\\synthetic";
    std::wstring op;
    bool ok = session.manifest().RecordOperationIntent(dir, op).ok;
    pm::jobs::JobManager job;
    DWORD error = 0;
    ok = ok && job.Create(session, L"barrier", error);
    pm::runtime::RollbackHandlers handlers;
    bool touched_directory = false;
    handlers.on_kill_job = [](const auto&, DWORD& e) { e = ERROR_ACCESS_DENIED; return false; };
    handlers.on_delete_dir = [&](const auto&, DWORD&) { touched_directory = true; return true; };
    auto failed = session.manifest().ExecutePendingRollbacks(handlers);
    ok = ok && !failed.all_succeeded && !touched_directory &&
        session.manifest().session_state() == SessionState::Cleaning;
    // Future operations are rejected by the phase-3 coordinator, never guessed.
    ok = ok && !pm::guard::CleanupSession(session, error) && error == ERROR_NOT_SUPPORTED;
    handlers.on_kill_job = [](const auto& operation, DWORD& e) {
        return pm::jobs::JobManager::TerminateAndWait(operation.job_name, e);
    };
    ok = ok && session.manifest().ExecutePendingRollbacks(handlers).all_succeeded;
    if (ok) RemoveSuccessfulSession(init.session_id);
    return ok;
}

bool Rejections() {
    SessionManager session;
    const auto init = session.InitializeNewSession();
    if (!init.ok) return false;
    DWORD error = 0;
    pm::jobs::JobManager first, duplicate;
    bool ok = !first.Create(session, L"../invalid", error) && first.Create(session, L"one", error) &&
        !duplicate.Create(session, L"one", error) && error == ERROR_ALREADY_EXISTS;
    PROCESS_INFORMATION p{};
    ok = ok && !first.Launch(L"Z:\\PM_missing_GUID.exe", L"", L"", p, error);
    pm::guard::GuardProcess guard;
    ok = ok && !guard.Start(L"Z:\\PM_missing_GUID.exe", session, error);
    ok = ok && pm::guard::CleanupSession(session, error);
    if (ok) RemoveSuccessfulSession(init.session_id);
    return ok;
}
}
int wmain(int argc, wchar_t** argv) {
    wchar_t path[32768]{};
    if (!::GetModuleFileNameW(nullptr, path, 32768)) return 2;
    self = path;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--leaf") { ::Sleep(INFINITE); return 0; }
    if (argc > 2 && std::wstring_view(argv[1]) == L"--tree") return Tree(argv[2]);
    if (argc > 1 && std::wstring_view(argv[1]) == L"--controller") return Controller(argc, argv);
    if (argc != 2 && argc != 3) return 2;
    const std::wstring pm = argv[1];
    struct Test { const wchar_t* name; bool passed; };
    const std::vector<Test> tests = {
        {L"normal_once_active_guard_blocked_cpu_children_foreign", Lifecycle(pm, L"normal")},
        {L"abrupt_pm_death_children_terminated", Lifecycle(pm, L"crash")},
        {L"guard_death_pm_still_responsible", Lifecycle(pm, L"guard-dead")},
        {L"source_folder_loss_then_pm_crash", MissingSource(pm)},
        {L"simultaneous_children_exit", Lifecycle(pm, L"children-dead")},
        {L"kill_on_last_job_handle_close", KillOnLastClose()},
        {L"kill_failure_stops_restoration", FailureBarrier()},
        {L"launch_after_cleanup_rejected", RejectLaunchAfterCleanup()},
        {L"invalid_id_duplicate_job_missing_executable", Rejections()},
    };
    bool all = true;
    for (const auto& t : tests) { std::wprintf(L"[%ls] %ls\n", t.passed ? L"PASS" : L"FAIL", t.name); all &= t.passed; }
    if (argc == 3) {
        std::string json = "{\n  \"phase\": 3,\n  \"platform\": \"native_windows\",\n  \"scenarios\": [\n";
        for (std::size_t i = 0; i < tests.size(); ++i) {
            const std::wstring name = tests[i].name;
            json += "    {\"name\": \"" + std::string(name.begin(), name.end()) +
                "\", \"status\": \"" + (tests[i].passed ? "passed" : "failed") + "\"}" +
                (i + 1 == tests.size() ? "\n" : ",\n");
        }
        json += std::string("  ],\n  \"status\": \"") + (all ? "passed" : "failed") + "\"\n}\n";
        if (!pm::core::AtomicFile::WriteUtf8Text(argv[2], json, false).ok) return 3;
    }
    return all ? 0 : 1;
}
