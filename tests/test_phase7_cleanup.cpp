#include "cleanup/cleanup_module.h"
#include "core/path_resolver.h"
#include "guard/guard_process.h"
#include "jobs/job_manager.h"
#include "junctions/junction_manager.h"
#include "registry/registry_manager.h"
#include "runtime/session_lock.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using pm::cleanup::CleanupEngine;
using pm::cleanup::Mode;
using pm::runtime::SessionManager;
using pm::runtime::SessionState;
using pm::jobs::QuoteArgument;

constexpr DWORD kCooperativeExit = 37;
std::wstring self;
std::wstring pm_exe;
ULONGLONG fast_elapsed = 0;
ULONGLONG fast_job_exit = 0;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string(message) +
        " (Win32=" + std::to_string(::GetLastError()) + ")");
}

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct Child {
    PROCESS_INFORMATION info{};
    ~Child() {
        if (info.hProcess) {
            if (::WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
                ::TerminateProcess(info.hProcess, ERROR_PROCESS_ABORTED);
                ::WaitForSingleObject(info.hProcess, 5000);
            }
            ::CloseHandle(info.hProcess);
        }
        if (info.hThread) ::CloseHandle(info.hThread);
    }
    DWORD Wait() const {
        DWORD code = 0;
        Check(::WaitForSingleObject(info.hProcess, 15000) == WAIT_OBJECT_0 &&
              ::GetExitCodeProcess(info.hProcess, &code), "wait for synthetic process");
        return code;
    }
};

std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& path, const char* text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    Check(stream.good(), "write synthetic file");
}

std::wstring EventName(const std::wstring& id, const wchar_t* suffix) {
    return L"Local\\PM_Phase7_" + id + suffix;
}

void Spawn(Child& child, const std::wstring& args) {
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    auto command = QuoteArgument(self) + L" " + args;
    Check(::CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &start, &child.info), "spawn helper");
}

struct Fixture {
    const std::wstring id = SessionManager::GenerateSessionGuid();
    const std::wstring old_temp = SessionManager::QuerySystemTempDir();
    const fs::path root = fs::path(old_temp) / (L"pm7-" + id);
    const fs::path portable = root / L"portable";
    const fs::path host = root / L"host" / L"app";
    const fs::path target = portable / L"Data" / L"apps" / L"app";
    const fs::path session_dir = root / L"PortableManager" / L"Session" / id;
    const fs::path log = root / L"pmlog";
    const std::wstring key = L"Software\\PortableManagerPhase7-" + id;

    Fixture() {
        Check(!id.empty() && fs::create_directory(root), "create isolated temp root");
        fs::create_directories(host);
        fs::create_directories(target);
        Write(host / L"original.txt", "original host");
        Write(target / L"portable.txt", "portable content");
        Check(::SetEnvironmentVariableW(L"TEMP", root.c_str()) &&
              ::SetEnvironmentVariableW(L"TMP", root.c_str()), "redirect TEMP and TMP");
        SetValue(7);
    }
    ~Fixture() {
        ::RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        ::SetEnvironmentVariableW(L"TEMP", old_temp.c_str());
        ::SetEnvironmentVariableW(L"TMP", old_temp.c_str());
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    void SetValue(DWORD value) const {
        HKEY handle = nullptr;
        Check(::RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0,
                                KEY_SET_VALUE, nullptr, &handle, nullptr) == ERROR_SUCCESS,
              "create isolated HKCU key");
        const auto result = ::RegSetValueExW(handle, L"SyntheticValue", 0, REG_DWORD,
                                             reinterpret_cast<const BYTE*>(&value), sizeof(value));
        ::RegCloseKey(handle);
        Check(result == ERROR_SUCCESS, "set synthetic HKCU value");
    }
    void CheckRestored(const fs::path& portable_target) const {
        Check(Read(host / L"original.txt") == "original host" &&
              !fs::exists(host.wstring() + L".pmbkp"), "restore exact original directory");
        Check(Read(portable_target / L"portable.txt") == "portable content", "preserve portable data");
        DWORD value = 0, size = sizeof(value);
        Check(::RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"SyntheticValue", RRF_RT_REG_DWORD,
                             nullptr, &value, &size) == ERROR_SUCCESS && value == 7,
              "restore original HKCU value");
    }
};

void Prepare(SessionManager& session, const fs::path& root, const std::wstring& id) {
    const pm::core::PathResolver resolver((root / L"portable").wstring());
    DWORD error = 0;
    Check(pm::junctions::JunctionManager::Prepare(session, resolver, L"app",
        (root / L"host" / L"app").wstring(), L"Data\\apps\\app", error), "prepare owned Junction");
    const bool prepared = pm::registry::RegistryManager::Prepare(
        session, L"HKCU\\Software\\PortableManagerPhase7-" + id, error);
    ::SetLastError(error);
    Check(prepared, "prepare HKCU backup");
}

LRESULT CALLBACK WorkerWindow(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CLOSE) {
        const auto cooperative = ::GetWindowLongPtrW(window, GWLP_USERDATA);
        if (cooperative) ::PostQuitMessage(kCooperativeExit);
        return 0;
    }
    return ::DefWindowProcW(window, message, wp, lp);
}

int Worker(const std::wstring& event, bool cooperative) {
    WNDCLASSW cls{};
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.lpfnWndProc = WorkerWindow;
    cls.lpszClassName = L"PortableManagerPhase7Worker";
    Check(::RegisterClassW(&cls) != 0, "register worker window");
    const HWND window = ::CreateWindowW(cls.lpszClassName, L"PM synthetic worker",
        WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, cls.hInstance, nullptr);
    Check(window != nullptr, "create worker window");
    ::SetWindowLongPtrW(window, GWLP_USERDATA, cooperative ? 1 : 0);
    Handle ready(::OpenEventW(EVENT_MODIFY_STATE, FALSE, event.c_str()));
    Check(ready.value && ::SetEvent(ready.value), "signal worker ready");
    MSG msg{};
    BOOL received;
    while ((received = ::GetMessageW(&msg, nullptr, 0, 0)) > 0) ::DispatchMessageW(&msg);
    return received == 0 ? static_cast<int>(msg.wParam) : 2;
}

void LaunchWorker(Child& child, const std::wstring& event, bool cooperative,
                  pm::jobs::JobManager* job) {
    Handle ready(::CreateEventW(nullptr, TRUE, FALSE, event.c_str()));
    Check(ready.value, "create worker event");
    const auto args = L"--worker " + QuoteArgument(event) + (cooperative ? L" 1" : L" 0");
    if (job) {
        DWORD error = 0;
        Check(job->Launch(self, args, L"", child.info, error), "launch controlled worker");
    } else {
        Spawn(child, args);
    }
    Check(::WaitForSingleObject(ready.value, 10000) == WAIT_OBJECT_0, "wait for worker window");
}

ULONGLONG FileTicks(const FILETIME& time) {
    return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

void CloseMode(Mode mode, bool cooperative, DWORD expected) {
    Fixture fixture;
    SessionManager session;
    Check(session.InitializeSessionWithId(fixture.id).ok, "initialize session");
    Prepare(session, fixture.root, fixture.id);
    fixture.SetValue(99);
    pm::jobs::JobManager job;
    DWORD error = 0;
    Check(job.Create(session, L"app", error), "create controlled Job");
    Child managed, outsider;
    LaunchWorker(managed, EventName(fixture.id, L"_managed"), cooperative, &job);
    LaunchWorker(outsider, EventName(fixture.id, L"_outsider"), true, nullptr);
    const auto start = ::GetTickCount64();
    FILETIME started{};
    ::GetSystemTimeAsFileTime(&started);
    const auto result = CleanupEngine::Run(session, mode);
    const auto elapsed = ::GetTickCount64() - start;
    Check(result.ok, "close and restore");
    Check(managed.Wait() == expected && pm::jobs::JobManager::IsEmpty(job.name()), "drain only managed Job");
    Check(::WaitForSingleObject(outsider.info.hProcess, 0) == WAIT_TIMEOUT, "leave outsider alive");
    if (mode == Mode::Fast) {
        fast_elapsed = elapsed;
        FILETIME created{}, exited{}, kernel{}, user{};
        Check(::GetProcessTimes(managed.info.hProcess, &created, &exited, &kernel, &user) &&
              FileTicks(exited) >= FileTicks(started), "read controlled process exit time");
        fast_job_exit = (FileTicks(exited) - FileTicks(started)) / 10000;
        Check(fast_job_exit >= pm::cleanup::kFastGraceMs - 16 && fast_job_exit < 1500,
              "enforce the fast cooperative deadline independently of restoration time");
    }
    fixture.CheckRestored(fixture.target);
    Check(!fs::exists(fixture.session_dir) && !fs::exists(fixture.log) &&
          !fs::exists(fixture.root / L"pmlog.txt"), "remove own temporaries and omit successful log");
    Check(CleanupEngine::Run(session, mode).ok && !fs::exists(fixture.log), "idempotent after finalization");
}

void Residue(bool foreign_log) {
    Fixture fixture;
    SessionManager session;
    Check(session.InitializeSessionWithId(fixture.id).ok, "initialize residue session");
    Prepare(session, fixture.root, fixture.id);
    fixture.SetValue(99);
    const auto displaced = fixture.host.parent_path() / L"displaced-link";
    fs::rename(fixture.host, displaced);
    fs::create_directory(fixture.host);
    Write(fixture.host / L"foreign.txt", "do not touch");
    if (foreign_log) Write(fixture.log, "unrelated file");
    const auto result = CleanupEngine::Run(session, Mode::Critical);
    Check(!result.ok && fs::exists(fixture.host.wstring() + L".pmbkp") &&
          Read(fixture.host / L"foreign.txt") == "do not touch", "retain blocked backup and foreign directory");
    Check(session.manifest().session_state() == SessionState::Cleaning, "keep failed session pending");
    const auto log = Read(fixture.log);
    if (foreign_log) {
        Check(result.log_error == ERROR_INVALID_DATA && log == "unrelated file", "preserve foreign pmlog");
    } else {
        Check(result.log_error == ERROR_SUCCESS && log.find("REMOVE_JUNCTION") != std::string::npos &&
              log.find("SyntheticValue") == std::string::npos &&
              !fs::exists(fixture.root / L"pmlog.txt"), "log only pending operation, path and code");
    }
    fs::remove(fixture.host / L"foreign.txt");
    fs::remove(fixture.host);
    fs::rename(displaced, fixture.host);
    Check(CleanupEngine::Run(session, Mode::Critical).ok && Read(fixture.log) == log, "retry and preserve earlier log");
    fixture.CheckRestored(fixture.target);
}

void UnreleasedRegistryBackup() {
    Fixture fixture;
    SessionManager session;
    Check(session.InitializeSessionWithId(fixture.id).ok, "initialize registry retry session");
    Prepare(session, fixture.root, fixture.id);
    fixture.SetValue(99);
    const auto backup = session.manifest().operations().back().backup_host_path;
    const auto original = Read(backup);
    {
        Handle blocked(::CreateFileW(backup.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr));
        Check(blocked.value != INVALID_HANDLE_VALUE, "lock registry backup against removal");
        const auto result = CleanupEngine::Run(session, Mode::Critical);
        Check(!result.ok && !original.empty() && Read(backup) == original &&
              Read(fixture.log).find("RELEASE_REG_BACKUP") != std::string::npos,
              "retain verified backup on release failure");
    }
    fixture.CheckRestored(fixture.target);
    fixture.SetValue(101);
    Check(CleanupEngine::Run(session, Mode::Critical).ok && !fs::exists(backup),
          "retry only backup release");
    DWORD value = 0, size = sizeof(value);
    Check(::RegGetValueW(HKEY_CURRENT_USER, fixture.key.c_str(), L"SyntheticValue", RRF_RT_REG_DWORD,
                         nullptr, &value, &size) == ERROR_SUCCESS && value == 101,
          "do not redo completed registry rollback");
}

void JobFailureBarrier() {
    Fixture fixture;
    SessionManager session;
    Check(session.InitializeSessionWithId(fixture.id).ok, "initialize Job barrier session");
    Prepare(session, fixture.root, fixture.id);
    fixture.SetValue(99);
    pm::jobs::JobManager job;
    DWORD error = 0;
    Check(job.Create(session, L"app", error), "create Job for barrier");
    Child worker;
    LaunchWorker(worker, EventName(fixture.id, L"_barrier"), true, &job);
    Handle security(::OpenJobObjectW(READ_CONTROL | WRITE_DAC, FALSE, job.name().c_str()));
    Check(security.value, "open test Job security");
    alignas(SECURITY_DESCRIPTOR) BYTE original[4096]{};
    DWORD needed = 0;
    Check(::GetKernelObjectSecurity(security.value, DACL_SECURITY_INFORMATION,
                                   original, sizeof(original), &needed), "save test Job DACL");
    alignas(DWORD) BYTE everyone[SECURITY_MAX_SID_SIZE]{};
    DWORD sid_size = sizeof(everyone);
    alignas(ACL) BYTE acl_buffer[256]{};
    auto* acl = reinterpret_cast<ACL*>(acl_buffer);
    SECURITY_DESCRIPTOR restricted{};
    Check(::CreateWellKnownSid(WinWorldSid, nullptr, everyone, &sid_size) &&
          ::InitializeAcl(acl, sizeof(acl_buffer), ACL_REVISION) &&
          ::AddAccessDeniedAce(acl, ACL_REVISION, JOB_OBJECT_TERMINATE, everyone) &&
          ::AddAccessAllowedAce(acl, ACL_REVISION, JOB_OBJECT_ALL_ACCESS, everyone) &&
          ::InitializeSecurityDescriptor(&restricted, SECURITY_DESCRIPTOR_REVISION) &&
          ::SetSecurityDescriptorDacl(&restricted, TRUE, acl, FALSE) &&
          ::SetKernelObjectSecurity(security.value, DACL_SECURITY_INFORMATION, &restricted),
          "deny termination for named test Job");
    const auto blocked = CleanupEngine::Run(session, Mode::Critical);
    Check(::SetKernelObjectSecurity(security.value, DACL_SECURITY_INFORMATION, original),
          "restore test Job DACL");
    DWORD value = 0, size = sizeof(value);
    Check(!blocked.ok && blocked.error == ERROR_ACCESS_DENIED &&
          ::WaitForSingleObject(worker.info.hProcess, 0) == WAIT_TIMEOUT &&
          Read(fixture.host / L"portable.txt") == "portable content" &&
          ::RegGetValueW(HKEY_CURRENT_USER, fixture.key.c_str(), L"SyntheticValue", RRF_RT_REG_DWORD,
                         nullptr, &value, &size) == ERROR_SUCCESS && value == 99 &&
          Read(fixture.log).find("KILL_JOB") != std::string::npos,
          "block Junction and HKCU restore while the Job is not drained");
    Check(CleanupEngine::Run(session, Mode::Critical).ok &&
          worker.Wait() == ERROR_PROCESS_ABORTED, "retry and drain Job");
    fixture.CheckRestored(fixture.target);
}

void OwnTemporariesOnly() {
    Fixture fixture;
    SessionManager session;
    Check(session.InitializeSessionWithId(fixture.id).ok, "initialize temporary ownership case");
    const auto owned = fixture.session_dir / L"session.ini.pmtmp.1.2.3.4";
    const auto foreign = fixture.session_dir / L"session.ini.pmtmp.4.3.2.1";
    fs::copy_file(session.manifest_path(), owned);
    Write(foreign, "foreign content even if the name matches");
    Check(CleanupEngine::Run(session, Mode::Critical).ok &&
          !fs::exists(owned) && Read(foreign) == "foreign content even if the name matches" &&
          !fs::exists(fixture.log), "remove only verified owned temporary file");
}

int Owner(const fs::path& root, const std::wstring& id, bool crash) {
    SessionManager session;
    Check(session.InitializeSessionWithId(id).ok, "initialize owner session");
    const auto origin = (root / L"portable" / L"pml.exe").wstring();
    pm::guard::GuardProcess guard;
    DWORD error = 0;
    Check(guard.Start(origin, session, error), "start actual PMGuard shadow");
    Prepare(session, root, id);
    pm::jobs::JobManager job;
    Check(job.Create(session, L"app", error), "create owner Job");
    Child worker;
    LaunchWorker(worker, EventName(id, L"_owner_worker"), true, &job);
    Write(root / L"guard.pid", std::to_string(::GetProcessId(guard.handle())).c_str());
    Handle ready(::OpenEventW(EVENT_MODIFY_STATE, FALSE, EventName(id, L"_owner_ready").c_str()));
    Check(ready.value && ::SetEvent(ready.value), "signal owner ready");
    if (crash) {
        pm::runtime::SessionLock lock(session.session_mutex_name());
        Check(lock.ok() && session.TransitionState(SessionState::Cleaning).ok, "persist partial cleanup");
        Handle abandon(::OpenEventW(SYNCHRONIZE, FALSE, EventName(id, L"_abandon").c_str()));
        Check(abandon.value && ::WaitForSingleObject(abandon.value, 10000) == WAIT_OBJECT_0, "wait to abandon mutex");
        ::TerminateProcess(::GetCurrentProcess(), 78);
        return 2;
    }
    Check(pm::guard::WaitForReleaseOrOriginLoss(origin, nullptr, 10000, error) ==
          pm::guard::OriginWaitResult::OriginLost, "detect synthetic loss of origin");
    Check(CleanupEngine::Run(session, Mode::Critical, pm::cleanup::Artifacts::Remove, &guard).ok,
          "restore from live PM");
    return 0;
}

void Emergency(bool crash) {
    Fixture fixture;
    fs::copy_file(pm_exe, fixture.portable / L"pml.exe");
    Handle ready(::CreateEventW(nullptr, TRUE, FALSE, EventName(fixture.id, L"_owner_ready").c_str()));
    Handle abandon(::CreateEventW(nullptr, TRUE, FALSE, EventName(fixture.id, L"_abandon").c_str()));
    Child owner;
    Spawn(owner, L"--owner " + QuoteArgument(fixture.root.wstring()) + L" " + QuoteArgument(fixture.id) +
          (crash ? L" 1" : L" 0"));
    HANDLE waits[]{ready.value, owner.info.hProcess};
    Check(::WaitForMultipleObjects(2, waits, FALSE, 12000) == WAIT_OBJECT_0, "owner and guard ready");
    Handle guard(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               std::stoul(Read(fixture.root / L"guard.pid"))));
    Check(guard.value && ::WaitForSingleObject(guard.value, 0) == WAIT_TIMEOUT &&
          fs::exists(fixture.session_dir / L"session.ini"), "guard stays idle while PM is active");
    const auto shadow = fixture.session_dir / L"pml.exe";
    std::wstring image(32768, L'\\0');
    DWORD length = static_cast<DWORD>(image.size());
    Check(::QueryFullProcessImageNameW(guard.value, 0, image.data(), &length) != FALSE,
          "query PMG executable");
    image.resize(length);
    Check(fs::equivalent(shadow, fs::path(image)), "run the session's pml.exe as PMG");
    fixture.SetValue(99);
    const auto disconnected = fixture.root / L"disconnected";
    fs::rename(fixture.portable, disconnected);
    if (crash) Check(::SetEvent(abandon.value), "crash owner after losing origin");
    Check(owner.Wait() == (crash ? 78U : 0U), "expected owner exit");
    DWORD guard_code = 9;
    Check(::WaitForSingleObject(guard.value, 15000) == WAIT_OBJECT_0 &&
          ::GetExitCodeProcess(guard.value, &guard_code) && guard_code == (crash ? 7U : 0U),
          "PMGuard exit after recovery");
    fixture.CheckRestored(disconnected / L"Data" / L"apps" / L"app");
    Check(!fs::exists(fixture.portable), "no writes at lost origin");
    if (crash) {
        const auto source_bytes = Read(disconnected / L"pml.exe");
        Check(!source_bytes.empty() && Read(shadow) == source_bytes, "PMG is a copy of PML");
        SessionManager recovered;
        Check(recovered.OpenExistingSession(fixture.id).ok &&
              recovered.manifest().session_state() == SessionState::Cleaned &&
              Read(fixture.log).find("REMOVE_GUARD_SHADOW") != std::string::npos &&
              Read(fixture.log).find("pml.exe") != std::string::npos && fs::exists(shadow),
              "record the sole remaining mapped guard image after crash recovery");
    } else {
        Check(!fs::exists(fixture.session_dir) && !fs::exists(fixture.log),
              "live PM retires guard and removes session");
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        self = pm::core::PathResolver::QueryExecutablePath();
        if (argc == 4 && std::wstring_view(argv[1]) == L"--worker")
            return Worker(argv[2], std::wstring_view(argv[3]) == L"1");
        if (argc == 5 && std::wstring_view(argv[1]) == L"--owner")
            return Owner(argv[2], argv[3], std::wstring_view(argv[4]) == L"1");
        if (argc != 3) return 2;
        pm_exe = argv[1];
        std::ofstream evidence(fs::path(argv[2]), std::ios::binary);
        Check(evidence.good(), "open Phase 7 evidence");
        evidence << "{\n  \"cases\": {\n";
        unsigned failures = 0, count = 0;
        const auto run = [&](const char* name, const auto& test) {
            const char* status = "passed";
            try { test(); }
            catch (const std::exception& error) {
                std::fprintf(stderr, "%s: %s\n", name, error.what());
                ++failures;
                status = "failed";
            }
            evidence << (count++ ? ",\n" : "") << "    \"" << name << "\": \"" << status << "\"";
            std::printf("%s: %s\n", name, status);
        };
        run("normal_cooperative_and_outsider", [] { CloseMode(Mode::Normal, true, kCooperativeExit); });
        run("fast_forces_after_200_ms", [] { CloseMode(Mode::Fast, false, ERROR_PROCESS_ABORTED); });
        run("critical_without_cooperative_close", [] { CloseMode(Mode::Critical, true, ERROR_PROCESS_ABORTED); });
        run("foreign_directory_log_backup_retry", [] { Residue(false); });
        run("preserve_foreign_pmlog", [] { Residue(true); });
        run("registry_backup_release_retry", UnreleasedRegistryBackup);
        run("job_failure_blocks_host_restoration", JobFailureBarrier);
        run("remove_only_owned_temporaries", OwnTemporariesOnly);
        run("live_pm_synthetic_origin_loss", [] { Emergency(false); });
        run("pmguard_recovers_abandoned_cleanup", [] { Emergency(true); });
        evidence << "\n  },\n  \"fast_total_ms\": " << fast_elapsed
                 << ",\n  \"fast_job_exit_ms\": " << fast_job_exit
                 << ",\n  \"physical_usb_disconnect\": \"not_run\",\n  \"status\": \""
                 << (failures ? "failed" : "passed") << "\"\n}\n";
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
