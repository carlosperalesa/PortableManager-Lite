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
std::wstring context, diagnostic_file, artifact_dir;
std::string diagnostic_log;
bool Check(bool ok, const char* step, ULONGLONG actual = 0, ULONGLONG expected = 0) {
    const DWORD error = ::GetLastError();
    const std::string scope(context.begin(), context.end());
    const std::string line = "[CHECK] " + scope + " step=" + step + " status=" +
        (ok ? "passed" : "failed") + " actual=" + std::to_string(actual) +
        " expected=" + std::to_string(expected) + " last_error=" + std::to_string(error) + "\n";
    diagnostic_log += line;
    const std::wstring wide(line.begin(), line.end());
    std::fwprintf(stdout, L"%ls", wide.c_str()); std::fflush(stdout);
    if (!diagnostic_file.empty())
        pm::core::AtomicFile::WriteUtf8Text(diagnostic_file, diagnostic_log, false);
    return ok;
}
bool Value(const char* step, ULONGLONG actual, ULONGLONG expected) {
    return Check(actual == expected, step, actual, expected);
}
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
    return Value("wait_process_exit", ::WaitForSingleObject(process, 15000), WAIT_OBJECT_0) &&
        Check(::GetExitCodeProcess(process, &code) != FALSE, "get_process_exit_code") &&
        Value("process_exit_code", code, expected);
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
    context = L"controller_" + mode + L"_" + id;
    HANDLE ready = Event(id, L"_ready"), release = Event(id, L"_release");
    SessionManager session;
    DWORD error = 0;
    if (!Check(session.InitializeSessionWithId(id).ok, "initialize_session")) return 3;
    diagnostic_file = session.session_dir() + L"\\controller-diagnostic.txt";
    pm::jobs::JobManager job;
    if (!Check(job.Create(session, L"synthetic", error), "create_job", error)) return 4;
    pm::guard::GuardProcess guard;
    if (!Check(guard.Start(pm, session, error), "start_guard", error)) return 5;
    PROCESS_INFORMATION worker{};
    if (!Check(job.Launch(self, L"--tree " + Q(id), SessionManager::QuerySystemTempDir(), worker, error), "launch_tree", error)) return 6;
    HANDLE tree_ready = Event(id, L"_tree");
    if (!Value("tree_ready", ::WaitForSingleObject(tree_ready, 10000), WAIT_OBJECT_0)) return 7;
    ::CloseHandle(tree_ready);
    if (!Check(session.TransitionState(SessionState::Active).ok, "activate_session")) return 8;
    const std::string info = std::to_string(::GetProcessId(guard.handle())) + " " + std::to_string(worker.dwProcessId);
    if (!Check(pm::core::AtomicFile::WriteUtf8Text(session.session_dir() + L"\\test-info.txt", info, false).ok, "publish_pids")) return 9;
    ::SetEvent(ready);
    ::WaitForSingleObject(release, INFINITE);
    if (mode == L"normal" || mode == L"guard-dead" || mode == L"children-dead") {
        // Calling twice must perform no second rollback after CLEANED.
        if (!Check(pm::guard::CleanupSession(session, error), "cleanup_first", error) ||
            !Check(pm::guard::CleanupSession(session, error), "cleanup_repeat", error)) return 10;
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
    ::DeleteFileW((dir + L"\\controller-diagnostic.txt").c_str());
    ::DeleteFileW((dir + L"\\session.ini").c_str());
    ::RemoveDirectoryW(dir.c_str());
}
bool Lifecycle(const std::wstring& pm, const std::wstring& mode, bool remove_source = false) {
    const std::wstring id = SessionManager::GenerateSessionGuid();
    HANDLE ready = Event(id, L"_ready"), release = Event(id, L"_release");
    PROCESS_INFORMATION owner{}, outsider{};
    HANDLE guard = nullptr, worker = nullptr, job = nullptr;
    context = mode + (remove_source ? L"_source_loss_" : L"_") + id;
    auto exercise = [&]() {
        if (!Check(ready && release, "create_events")) return false;
        if (!Check(Spawn(self, L"--leaf", outsider), "spawn_outsider")) return false;
        if (!Check(Spawn(self, L"--controller " + Q(id) + L" " + Q(pm) + L" " + Q(mode), owner), "spawn_owner")) return false;
        HANDLE startup[] = {ready, owner.hProcess};
        if (!Value("owner_ready", ::WaitForMultipleObjects(2, startup, FALSE, 15000), WAIT_OBJECT_0)) return false;
        auto info = pm::core::AtomicFile::ReadUtf8Text(SessionPath(id) + L"\\test-info.txt");
        unsigned long guard_id = 0, worker_id = 0;
        if (!Check(info.ok && std::sscanf(info.content.c_str(), "%lu %lu", &guard_id, &worker_id) == 2, "read_pids")) return false;
        guard = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, guard_id);
        if (!Check(guard != nullptr, "open_guard")) return false;
        worker = ::OpenProcess(SYNCHRONIZE, FALSE, worker_id);
        if (!Check(worker != nullptr, "open_worker")) return false;
        job = ::OpenJobObjectW(JOB_OBJECT_QUERY | JOB_OBJECT_TERMINATE, FALSE,
            pm::jobs::JobManager::MakeName(id, L"synthetic").c_str());
        if (!Check(job != nullptr, "open_job")) return false;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!Check(::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) != FALSE, "query_active_job")) return false;
        if (!Value("active_processes_before", accounting.ActiveProcesses, 2)) return false;
        const ULONGLONG before = Cpu(guard);
        ::Sleep(1200);
        const ULONGLONG after = Cpu(guard);
        if (!Check(before != ~0ULL && after >= before && (after - before) <= 200000,
                   "blocked_guard_cpu_1200ms", after - before, 200000)) return false;
        if (!Value("guard_alive", ::WaitForSingleObject(guard, 0), WAIT_TIMEOUT)) return false;
        if (!Value("worker_alive", ::WaitForSingleObject(worker, 0), WAIT_TIMEOUT)) return false;
        SessionManager observed;
        if (!Check(observed.OpenExistingSession(id).ok, "open_active_manifest")) return false;
        if (!Value("active_session_state", static_cast<unsigned>(observed.manifest().session_state()), static_cast<unsigned>(SessionState::Active))) return false;
        if (remove_source) {
            const std::wstring source_dir = pm.substr(0, pm.find_last_of(L"\\/"));
            if (!Check(::DeleteFileW(pm.c_str()) != FALSE, "remove_source_executable") ||
                !Check(::RemoveDirectoryW(source_dir.c_str()) != FALSE, "remove_source_directory")) return false;
        }
        if (mode == L"guard-dead") {
            if (!Check(::TerminateProcess(guard, 88) != FALSE, "terminate_guard") || !WaitExit(guard, 88)) return false;
        }
        if (mode == L"children-dead") {
            if (!Check(::TerminateJobObject(job, 89) != FALSE, "terminate_children") ||
                !Value("children_exit", ::WaitForSingleObject(worker, 10000), WAIT_OBJECT_0) ||
                !Value("owner_survives_children", ::WaitForSingleObject(owner.hProcess, 0), WAIT_TIMEOUT) ||
                !Value("guard_survives_children", ::WaitForSingleObject(guard, 0), WAIT_TIMEOUT)) return false;
        }
        if (!Check(::SetEvent(release) != FALSE, "release_owner")) return false;
        if (!WaitExit(owner.hProcess, mode == L"crash" ? 77 : 0)) return false;
        if (mode != L"guard-dead" && !WaitExit(guard)) return false;
        if (!Value("worker_terminated", ::WaitForSingleObject(worker, 10000), WAIT_OBJECT_0)) return false;
        if (!Check(::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) != FALSE, "query_drained_job")) return false;
        if (!Value("active_processes_after", accounting.ActiveProcesses, 0)) return false;
        if (!Check(observed.OpenExistingSession(id).ok, "open_final_manifest")) return false;
        if (!Value("final_session_state", static_cast<unsigned>(observed.manifest().session_state()), static_cast<unsigned>(SessionState::Cleaned))) return false;
        return Value("outsider_survives", ::WaitForSingleObject(outsider.hProcess, 0), WAIT_TIMEOUT);
    };
    const bool ok = exercise();
    // Snapshot BEFORE failure containment, so cleanup cannot mask the failing state.
    if (!artifact_dir.empty()) {
        const std::wstring prefix = artifact_dir + L"\\phase3_" + id;
        ::CopyFileW((SessionPath(id) + L"\\session.ini").c_str(), (prefix + L"_manifest.ini").c_str(), TRUE);
        ::CopyFileW((SessionPath(id) + L"\\controller-diagnostic.txt").c_str(), (prefix + L"_controller.txt").c_str(), TRUE);
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
    if (argc == 3) {
        const std::wstring output = argv[2];
        const auto slash = output.find_last_of(L"\\/");
        if (slash != std::wstring::npos) artifact_dir = output.substr(0, slash);
    }
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
        if (!pm::core::AtomicFile::WriteUtf8Text(std::wstring(argv[2]) + L".diagnostics.txt", diagnostic_log, false).ok) return 3;
    }
    return all ? 0 : 1;
}
