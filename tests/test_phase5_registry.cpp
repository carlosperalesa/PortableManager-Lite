#include "core/path_resolver.h"
#include "guard/guard_process.h"
#include "jobs/job_manager.h"
#include "registry/registry_manager.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
namespace fs = std::filesystem;
using pm::guard::CleanupSession;
using pm::jobs::JobManager;
using pm::jobs::QuoteArgument;
using pm::registry::RegistryManager;
using pm::runtime::OperationState;
using pm::runtime::SessionManager;
using pm::runtime::SessionState;

void Check(bool ok, const char* step, DWORD code = 0) {
    if (!ok) throw std::runtime_error(std::string(step) + " (Win32 " + std::to_string(code) + ")");
}

struct Close {
    void operator()(HANDLE handle) const { ::CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, Close>;

struct CloseKey {
    void operator()(HKEY key) const { ::RegCloseKey(key); }
};
using Key = std::unique_ptr<std::remove_pointer_t<HKEY>, CloseKey>;

std::wstring Relative(const std::wstring& id) {
    return L"Software\\PM_Fase05_" + id;
}

std::wstring Full(const std::wstring& id) {
    return L"HKCU\\" + Relative(id);
}

std::wstring Ready(const std::wstring& id) {
    return L"Local\\PM_Fase05_Ready_" + id;
}

bool Exists(const std::wstring& path) {
    HKEY raw = nullptr;
    const auto status = ::RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &raw);
    Key key(raw);
    if (status == ERROR_FILE_NOT_FOUND) return false;
    Check(status == ERROR_SUCCESS, "open synthetic key", status);
    return true;
}

Key Create(const std::wstring& path) {
    HKEY raw = nullptr;
    const auto status = ::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                                          KEY_READ | KEY_WRITE | DELETE, nullptr, &raw, nullptr);
    Check(status == ERROR_SUCCESS, "create synthetic key", status);
    return Key(raw);
}

template<class T>
void Put(HKEY key, const wchar_t* name, DWORD type, const T& data) {
    const auto status = ::RegSetValueExW(key, name, 0, type,
                                         reinterpret_cast<const BYTE*>(&data), sizeof(data));
    Check(status == ERROR_SUCCESS, "write synthetic value", status);
}

template<class T>
void Expect(HKEY key, const wchar_t* name, DWORD type, const T& expected) {
    DWORD actual_type = 0;
    std::array<BYTE, sizeof(T)> actual{};
    DWORD size = actual.size();
    const auto status = ::RegQueryValueExW(key, name, nullptr, &actual_type, actual.data(), &size);
    Check(status == ERROR_SUCCESS && actual_type == type && size == sizeof(T) &&
          std::memcmp(actual.data(), &expected, sizeof(T)) == 0, "value bytes and type restored", status);
}

constexpr wchar_t kText[] = L"Configuraci\u00f3n \"sint\u00e9tica\" \\ inicio";
constexpr wchar_t kMulti[] = L"uno\0dos\0";
constexpr DWORD kNumber = 49;
constexpr ULONGLONG kWideNumber = 0x12345678abcdef01ull;
constexpr BYTE kBinary[] = {0, 1, 128, 255, 0};
constexpr auto kLongBinary = [] {
    std::array<BYTE, 256> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<BYTE>(i);
    return bytes;
}();

struct Fixture {
    const std::wstring id = SessionManager::GenerateSessionGuid();
    const std::wstring key = Relative(id);
    const std::wstring sibling = key + L"_sentinel";
    SessionManager session;

    Fixture() {
        Check(!id.empty(), "fixture GUID");
        Check(session.InitializeSessionWithId(id).ok, "initialize session");
        Put(Create(sibling).get(), L"sentinel", REG_DWORD, kNumber);
    }

    ~Fixture() {
        ::RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        ::RegDeleteTreeW(HKEY_CURRENT_USER, sibling.c_str());
        // This directory was created by InitializeSessionWithId for this GUID.
        // Preserve the evidence for any test that fails before finalization.
    }

    void Prepare() {
        DWORD error = 0;
        const bool prepared = RegistryManager::Prepare(session, Full(id), error);
        Check(prepared, "prepare synthetic registry operation", error);
        Check(session.manifest().operations().size() == 1 &&
              session.manifest().operations()[0].state == OperationState::IntentRecorded,
              "intent precedes child writes");
    }

    std::wstring Backup() const { return session.manifest().operations().front().backup_host_path; }

    void VerifySibling() const {
        Check(Exists(sibling), "sibling preserved");
        Expect(Create(sibling).get(), L"sentinel", REG_DWORD, kNumber);
    }

    void Finalize() {
        DWORD error = 0;
        const bool finalized = session.FinalizeSessionAndCleanupOwnFiles(true, error);
        Check(finalized, "finalize own session", error);
        VerifySibling();
    }
};

void Populate(const std::wstring& root) {
    const Key key = Create(root);
    Put(key.get(), nullptr, REG_SZ, kText);
    Put(key.get(), L"Number", REG_DWORD, kNumber);
    Put(key.get(), L"Multi", REG_MULTI_SZ, kMulti);
    Put(key.get(), L"WideNumber", REG_QWORD, kWideNumber);
    Put(key.get(), L"LongBinary", REG_BINARY, kLongBinary);
    Put(Create(root + L"\\Original\\Nested").get(), L"Binary", REG_BINARY, kBinary);
    Create(root + L"\\Empty");
}

void Dirty(const std::wstring& root) {
    const Key key = Create(root);
    Put(key.get(), L"Number", REG_SZ, kText);
    Put(key.get(), L"Added", REG_DWORD, kNumber);
    const auto removed = ::RegDeleteTreeW(key.get(), L"Original");
    Check(removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND, "remove original synthetic subtree", removed);
    Create(root + L"\\New");
}

void VerifyOriginal(const std::wstring& root) {
    Check(Exists(root) && Exists(root + L"\\Original\\Nested") && Exists(root + L"\\Empty") &&
          !Exists(root + L"\\New"), "original subkeys restored");
    const Key key = Create(root);
    Expect(key.get(), nullptr, REG_SZ, kText);
    Expect(key.get(), L"Number", REG_DWORD, kNumber);
    Expect(key.get(), L"Multi", REG_MULTI_SZ, kMulti);
    Expect(key.get(), L"WideNumber", REG_QWORD, kWideNumber);
    Expect(key.get(), L"LongBinary", REG_BINARY, kLongBinary);
    Expect(Create(root + L"\\Original\\Nested").get(), L"Binary", REG_BINARY, kBinary);
    Check(::RegQueryValueExW(key.get(), L"Added", nullptr, nullptr, nullptr, nullptr) ==
          ERROR_FILE_NOT_FOUND, "added value removed");
}

void Clean(SessionManager& session) {
    DWORD error = 0;
    const bool cleaned = CleanupSession(session, error);
    Check(cleaned, "shared cleanup", error);
    Check(session.manifest().session_state() == SessionState::Cleaned, "persist CLEANED");
}

void RoundTrip(bool original) {
    Fixture f;
    if (original) Populate(f.key);
    f.Prepare();
    const auto backup = f.Backup();
    Check(original ? fs::exists(backup) : backup.empty(), "backup matches original existence");
    Dirty(f.key);
    Clean(f.session);
    Clean(f.session);
    if (original) {
        VerifyOriginal(f.key);
        Check(!fs::exists(backup), "discard backup after rollback");
    } else {
        Check(!Exists(f.key), "originally absent key removed");
    }
    f.Finalize();
}

void UnsafeInputs() {
    Fixture f;
    DWORD error = 0;
    for (const auto& key : {std::wstring(L"HKLM\\Software\\Synthetic"), std::wstring(L"HKCU\\Software"),
                            std::wstring(L"HKCU\\Software\\Classes\\Synthetic"), Full(f.id) + L"\\..",
                            Full(f.id) + L"\\MissingParent\\Leaf"}) {
        Check(!RegistryManager::Prepare(f.session, key, error), "reject unsafe or unattributed path");
    }
    Check(f.session.manifest().operations().empty(), "invalid paths leave no intent");
    f.Prepare();
    Check(!RegistryManager::Prepare(f.session, Full(f.id), error) && error == ERROR_ALREADY_EXISTS,
          "reject duplicate intent");
    Check(!RegistryManager::Prepare(f.session, Full(f.id) + L"\\Child", error) &&
          error == ERROR_ALREADY_EXISTS, "reject overlapping intent");
    Clean(f.session);
    f.Finalize();
}

void BackupFailure(bool tamper) {
    Fixture f;
    Populate(f.key);
    f.Prepare();
    const auto backup = f.Backup();
    Dirty(f.key);
    const auto parked = fs::path(f.session.session_dir()) / L"parked-original.reg";
    if (tamper) {
        Check(::MoveFileW(backup.c_str(), parked.c_str()), "park original backup");
        fs::copy_file(parked, backup);
        std::ofstream out(fs::path(backup), std::ios::binary | std::ios::app);
        const char extra[] = {'\r', 0, '\n', 0};
        Check(static_cast<bool>(out.write(extra, sizeof(extra))), "modify structurally valid backup");
    } else {
        Create(f.key + L"\\unsupported[key]");
    }
    DWORD error = 0;
    Check(!CleanupSession(f.session, error) && f.session.manifest().session_state() == SessionState::Cleaning &&
          f.session.manifest().operations().front().state == OperationState::Failed,
          "failure leaves durable pending state");
    Check(fs::exists(backup) && Exists(f.key + L"\\New"), "backup and dirty tree retained");
    if (tamper) {
        Check(::DeleteFileW(backup.c_str()) && ::MoveFileW(parked.c_str(), backup.c_str()), "return original backup");
    } else {
        Check(::RegDeleteTreeW(HKEY_CURRENT_USER, (f.key + L"\\unsupported[key]").c_str()) == ERROR_SUCCESS,
              "remove synthetic blocker");
        const auto& op = f.session.manifest().operations().front();
        const auto interrupted = fs::path(f.session.session_dir()) / (L"registry-check-" + op.id + L".reg");
        std::ofstream partial(interrupted, std::ios::binary);
        Check(static_cast<bool>(partial << "interrupted check"), "simulate interrupted verification export");
    }
    Clean(f.session);
    VerifyOriginal(f.key);
    f.Finalize();
}

void BackupReleaseRetry() {
    Fixture f;
    Populate(f.key);
    f.Prepare();
    const auto backup = f.Backup();
    Dirty(f.key);
    const HANDLE pinned = ::CreateFileW(backup.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
    Check(pinned != INVALID_HANDLE_VALUE, "block backup removal without blocking reads");
    Handle handle(pinned);
    DWORD error = 0;
    Check(!CleanupSession(f.session, error) && f.session.manifest().operations().front().state ==
          OperationState::RolledBack && f.session.manifest().session_state() == SessionState::Cleaning,
          "durable rollback before backup disposal");
    VerifyOriginal(f.key);
    Check(fs::exists(backup), "backup retained until it can be released");
    Put(Create(f.key).get(), L"AfterRollback", REG_DWORD, kNumber);
    handle.reset();
    Clean(f.session);
    Expect(Create(f.key).get(), L"AfterRollback", REG_DWORD, kNumber);
    f.Finalize();
}

void WaitExit(HANDLE process, DWORD expected) {
    DWORD code = 0;
    Check(::WaitForSingleObject(process, 15000) == WAIT_OBJECT_0, "wait for child exit");
    Check(::GetExitCodeProcess(process, &code) && code == expected, "child exit code", code);
}

int Writer(const std::wstring& id) {
    Check(id.size() == 36 && SessionManager::IsValidSessionId(id), "synthetic writer GUID");
    Dirty(Relative(id));
    Handle event(::OpenEventW(EVENT_MODIFY_STATE, FALSE, Ready(id).c_str()));
    Check(event && ::SetEvent(event.get()), "writer signals changes");
    ::Sleep(30000); // A live Job member until the test exercises cleanup.
    return 1;
}

void LaunchWriter(const std::wstring& self, SessionManager& session, JobManager& job,
                  Handle& process) {
    DWORD error = 0;
    const bool registered = job.Create(session, L"registry-test", error);
    Check(registered, "register synthetic job", error);
    PROCESS_INFORMATION pi{};
    const bool launched = job.Launch(self, L"--writer " + QuoteArgument(session.session_id()),
                                     SessionManager::QuerySystemTempDir(), pi, error,
                                     pm::jobs::ConsoleMode::Detached);
    Check(launched, "launch synthetic writer", error);
    process.reset(pi.hProcess);
    ::CloseHandle(pi.hThread);
}

void WaitForWriter(const Handle& event, const Handle& process) {
    const HANDLE waits[] = {event.get(), process.get()};
    Check(::WaitForMultipleObjects(2, waits, FALSE, 15000) == WAIT_OBJECT_0, "job writer changed registry");
}

void JobBeforeRegistry(const std::wstring& self) {
    Fixture f;
    Populate(f.key);
    f.Prepare();
    Handle ready(::CreateEventW(nullptr, TRUE, FALSE, Ready(f.id).c_str()));
    Check(ready != nullptr, "create writer event");
    JobManager job;
    Handle writer;
    LaunchWriter(self, f.session, job, writer);
    WaitForWriter(ready, writer);
    pm::runtime::RollbackHandlers handlers;
    handlers.on_kill_job = [](const auto&, DWORD& error) { error = ERROR_ACCESS_DENIED; return false; };
    bool restore_called = false;
    handlers.on_restore_reg = [&](const auto&, DWORD&) { restore_called = true; return false; };
    const auto blocked = f.session.manifest().ExecutePendingRollbacks(handlers);
    Check(!blocked.all_succeeded && !restore_called && fs::exists(f.Backup()) &&
          Exists(f.key + L"\\New"), "failed Job stop blocks registry rollback");
    Clean(f.session);
    WaitExit(writer.get(), ERROR_PROCESS_ABORTED);
    VerifyOriginal(f.key);
    f.Finalize();
}

int Owner(const std::wstring& self, const std::wstring& pm, const std::wstring& id) {
    SessionManager session;
    Check(session.OpenExistingSession(id).ok, "open synthetic owner session");
    pm::guard::GuardProcess guard;
    DWORD error = 0;
    const bool started = guard.Start(pm, session, error);
    Check(started, "start PMGuard", error);
    const auto ids = fs::path(session.session_dir()) / L"test-process-ids.txt";
    const bool prepared = RegistryManager::Prepare(session, Full(id), error);
    Check(prepared, "owner prepares registry", error);
    JobManager job;
    Handle writer;
    LaunchWriter(self, session, job, writer);
    std::ofstream pids(ids);
    Check(static_cast<bool>(pids << ::GetProcessId(guard.handle()) << ' ' << ::GetProcessId(writer.get())
                                << std::endl), "publish synthetic process ids");
    ::Sleep(30000);
    return 1;
}

void GuardAfterCrash(const std::wstring& self, const std::wstring& pm) {
    Fixture f;
    Populate(f.key);
    Handle ready(::CreateEventW(nullptr, TRUE, FALSE, Ready(f.id).c_str()));
    Check(ready != nullptr, "create writer event");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = QuoteArgument(self) + L" --owner " + QuoteArgument(pm) + L" " + QuoteArgument(f.id);
    Check(::CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &process), "spawn synthetic PM owner", ::GetLastError());
    Handle owner(process.hProcess), thread(process.hThread);
    const auto ids = fs::path(f.session.session_dir()) / L"test-process-ids.txt";
    try {
        WaitForWriter(ready, owner);
        DWORD guard_pid = 0, writer_pid = 0;
        for (unsigned attempt = 0; attempt < 100 && (!guard_pid || !writer_pid); ++attempt) {
            std::ifstream(ids) >> guard_pid >> writer_pid;
            if (!guard_pid || !writer_pid) ::Sleep(10);
        }
        Handle guard(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, guard_pid));
        Handle writer(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, writer_pid));
        Check(guard && writer, "open synthetic guard and writer handles");
        Check(::TerminateProcess(owner.get(), 89), "stop synthetic PM owner", ::GetLastError());
        WaitExit(owner.get(), 89);
        WaitExit(guard.get(), 0);
        Check(::WaitForSingleObject(writer.get(), 0) == WAIT_OBJECT_0, "Job writer gone before guard exits");
        Check(f.session.manifest().LoadFromDisk().ok &&
              f.session.manifest().session_state() == SessionState::Cleaned, "guard persists CLEANED");
        VerifyOriginal(f.key);
    } catch (...) {
        ::TerminateProcess(owner.get(), 99);
        ::WaitForSingleObject(owner.get(), 5000);
        DWORD error = 0;
        CleanupSession(f.session, error);
        throw;
    }
    ::DeleteFileW(ids.c_str());
    ::DeleteFileW((fs::path(f.session.session_dir()) / L"pm-guard.exe").c_str());
    f.Finalize();
}
}

int wmain(int argc, wchar_t* argv[]) {
    const auto self = pm::core::PathResolver::QueryExecutablePath();
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--writer") return Writer(argv[2]);
        if (argc == 4 && std::wstring_view(argv[1]) == L"--owner") return Owner(self, argv[2], argv[3]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (argc != 3) return 2;
    std::ofstream report(fs::path(argv[2]), std::ios::binary);
    if (!report) return 2;
    unsigned passed = 0, failed = 0;
    report << "{\n  \"scenarios\": [\n";
    auto run = [&](const char* name, auto scenario) {
        if (passed + failed) report << ",\n";
        try {
            scenario();
            ++passed;
            report << "    {\"name\": \"" << name << "\", \"status\": \"passed\"}";
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << name << ": " << error.what() << '\n';
            report << "    {\"name\": \"" << name << "\", \"status\": \"failed\"}";
        }
    };
    run("restore_values_types_and_subkeys", [] { RoundTrip(true); });
    run("remove_originally_absent_key", [] { RoundTrip(false); });
    run("reject_unsafe_and_overlapping_keys", UnsafeInputs);
    run("reject_tampered_backup_and_retry", [] { BackupFailure(true); });
    run("preserve_backup_on_unsafe_dirty_tree_and_retry", [] { BackupFailure(false); });
    run("persist_rollback_before_releasing_backup", BackupReleaseRetry);
    run("job_stop_blocks_then_permits_registry_rollback", [&] { JobBeforeRegistry(self); });
    run("pm_crash_guard_ends_job_and_restores_registry", [&] { GuardAfterCrash(self, argv[1]); });
    report << "\n  ],\n  \"passed\": " << passed << ",\n  \"failed\": " << failed
           << ",\n  \"status\": \"" << (failed ? "failed" : "passed") << "\"\n}\n";
    std::cout << "Fase 5: " << passed << " passed, " << failed << " failed\n";
    return failed || !report ? 1 : 0;
}
