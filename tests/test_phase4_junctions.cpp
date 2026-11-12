#include "core/path_resolver.h"
#include "guard/guard_process.h"
#include "junctions/junction_manager.h"

#include <bcrypt.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <winioctl.h>

namespace {
namespace fs = std::filesystem;
using pm::junctions::JunctionManager;
using pm::runtime::OperationState;
using pm::runtime::SessionManager;
using pm::runtime::SessionState;

constexpr wchar_t kData[] = L"Data\\apps\\synthetic";

void Check(bool ok, const char* step, DWORD error = ::GetLastError()) {
    if (!ok) throw std::runtime_error(std::string(step) + " (Win32 " + std::to_string(error) + ")");
}

void Write(const fs::path& path, const char* data) {
    std::ofstream out(path, std::ios::binary);
    Check(static_cast<bool>(out << data), "write synthetic data");
}

std::array<UCHAR, 32> Hash(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    Check(static_cast<bool>(in), "open fixture for SHA-256");
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    std::array<UCHAR, 32> digest{};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    Check(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0,
          "open SHA-256 provider");
    BCRYPT_HASH_HANDLE hash = nullptr;
    const bool ok = ::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
                    ::BCryptHashData(hash, reinterpret_cast<PUCHAR>(bytes.data()),
                                     static_cast<ULONG>(bytes.size()), 0) == 0 &&
                    ::BCryptFinishHash(hash, digest.data(), digest.size(), 0) == 0;
    if (hash) ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    Check(ok, "SHA-256");
    return digest;
}

struct Fixture {
    const std::wstring id = SessionManager::GenerateSessionGuid();
    const fs::path root = fs::path(SessionManager::QuerySystemTempDir()) / (L"PM_Fase04_" + id);
    const fs::path portable = root / L"portable";
    const fs::path target = portable / kData;
    const fs::path detached = target.parent_path() / L"detached";
    const fs::path host = root / L"exclusive";
    const fs::path backup = root / L"exclusive.pmbkp";
    const fs::path parked = root / L"parked";

    explicit Fixture(bool original = true) {
        Check(!id.empty(), "generate fixture GUID");
        Check(fs::create_directory(root), "create unique fixture root");
        fs::create_directories(target);
        Write(target / L"data.txt", "portable sentinel");
        if (original) {
            fs::create_directory(host);
            Write(host / L"original.txt", "host sentinel");
        }
    }

    ~Fixture() {
        // Each path is a synthetic fixture. Remove links themselves first, and
        // inspect the leaf before deleting any known file underneath it.
        for (const auto* directory : {&host, &backup, &parked, &target, &detached}) {
            if (::RemoveDirectoryW(directory->c_str())) continue;
            const DWORD attrs = ::GetFileAttributesW(directory->c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            for (const auto* name : {L"original.txt", L"data.txt", L"new.txt", L"foreign.txt"})
                ::DeleteFileW((*directory / name).c_str());
            ::RemoveDirectoryW(directory->c_str());
        }
        ::RemoveDirectoryW(target.parent_path().c_str());
        ::RemoveDirectoryW(target.parent_path().parent_path().c_str());
        ::RemoveDirectoryW(portable.c_str());
        ::DeleteFileW((root / L"guard.txt").c_str());
        ::RemoveDirectoryW(root.c_str());
    }

    bool Prepare(SessionManager& session, DWORD& error) const {
        return JunctionManager::Prepare(session, pm::core::PathResolver(portable.native()),
                                        L"synthetic", host.native(), kData, error);
    }

    std::wstring ReadyEvent() const { return L"Local\\PM_Fase04_Ready_" + id; }
    std::wstring ReleaseEvent() const { return L"Local\\PM_Fase04_Release_" + id; }
};

void StartSession(SessionManager& session, const Fixture& f) {
    Check(session.InitializeSessionWithId(f.id).ok, "initialize session");
}

void Apply(SessionManager& session, const Fixture& f) {
    DWORD error = 0;
    const bool applied = f.Prepare(session, error);
    Check(applied, "prepare junction", error);
    const DWORD attrs = ::GetFileAttributesW(f.host.c_str());
    Check(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT), "host is a reparse point");
    Check(session.manifest().operations().size() == 1 &&
          session.manifest().operations()[0].state == OperationState::Applied, "persist APPLIED");
}

void Cleanup(SessionManager& session) {
    DWORD error = 0;
    const bool cleaned = pm::guard::CleanupSession(session, error);
    Check(cleaned, "cleanup session", error);
    Check(session.manifest().session_state() == SessionState::Cleaned, "persist CLEANED");
}

void Finalize(SessionManager& session) {
    DWORD error = 0;
    const bool finalized = session.FinalizeSessionAndCleanupOwnFiles(true, error);
    Check(finalized, "remove completed session files", error);
}

void RoundTrip(bool original, bool orphaned) {
    Fixture f(original);
    SessionManager session;
    StartSession(session, f);
    const auto portable_hash = Hash(f.target / L"data.txt");
    const auto original_hash = original ? Hash(f.host / L"original.txt") : std::array<UCHAR, 32>{};
    Apply(session, f);
    Check(Hash(f.host / L"data.txt") == portable_hash, "read through junction");
    Write(f.host / L"new.txt", "written through junction");
    Check(Hash(f.host / L"new.txt") == Hash(f.target / L"new.txt"), "write to portable target");
    if (original) Check(Hash(f.backup / L"original.txt") == original_hash, "backup preserves SHA-256");
    if (orphaned) {
        Check(::MoveFileExW(f.target.c_str(), f.detached.c_str(), 0), "make target inaccessible");
        // Simulate a PM interruption before APPLIED persisted.
        const auto op_id = session.manifest().operations()[0].id;
        Check(session.manifest().UpdateOperationState(op_id, OperationState::IntentRecorded).ok,
              "persist interrupted state");
    }
    Cleanup(session);
    Cleanup(session);
    Check(!fs::exists(f.backup), "backup name released");
    if (original) {
        Check(Hash(f.host / L"original.txt") == original_hash, "restored exact original SHA-256");
        Check(!fs::exists(f.host / L"new.txt"), "portable write absent from restored host");
    } else {
        Check(::GetFileAttributesW(f.host.c_str()) == INVALID_FILE_ATTRIBUTES, "originally absent host restored");
    }
    const auto& location = orphaned ? f.detached : f.target;
    Check(Hash(location / L"data.txt") == portable_hash && fs::exists(location / L"new.txt"),
          "portable data intact after removal");
    Finalize(session);
}

void Preconditions() {
    Fixture f;
    SessionManager session;
    StartSession(session, f);
    const auto original = Hash(f.host / L"original.txt");
    Check(::MoveFileExW(f.target.c_str(), f.detached.c_str(), 0), "hide target before prepare");
    DWORD error = 0;
    Check(!f.Prepare(session, error) && error == ERROR_PATH_NOT_FOUND, "reject missing target");
    Check(::MoveFileExW(f.detached.c_str(), f.target.c_str(), 0), "restore synthetic target");
    Check(!JunctionManager::Prepare(session, pm::core::PathResolver(f.portable.native()),
                                    L"other", f.host.native(), kData, error), "reject other app data");
    fs::create_directory(f.backup);
    Write(f.backup / L"foreign.txt", "pre-existing backup sentinel");
    const auto previous_backup = Hash(f.backup / L"foreign.txt");
    Check(!f.Prepare(session, error) && error == ERROR_ALREADY_EXISTS, "reject pre-existing backup");
    Check(session.manifest().operations().empty(), "reject before writing intent");
    Check(Hash(f.host / L"original.txt") == original && Hash(f.backup / L"foreign.txt") == previous_backup,
          "pre-existing directories untouched");
    Cleanup(session);
    Finalize(session);
}

void ReplaceWithForeignJunction(const Fixture& f) {
    std::array<BYTE, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> data{};
    DWORD bytes = 0;
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS;
    HANDLE entry = ::CreateFileW(f.parked.c_str(), 0, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
    Check(entry != INVALID_HANDLE_VALUE, "open parked junction");
    const bool read = ::DeviceIoControl(entry, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                                       data.data(), data.size(), &bytes, nullptr);
    ::CloseHandle(entry);
    Check(read, "read exact reparse payload");
    Check(fs::create_directory(f.host), "create replacement directory");
    entry = ::CreateFileW(f.host.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, flags, nullptr);
    Check(entry != INVALID_HANDLE_VALUE, "open replacement directory");
    DWORD ignored = 0;
    const bool set = ::DeviceIoControl(entry, FSCTL_SET_REPARSE_POINT, data.data(), bytes,
                                      nullptr, 0, &ignored, nullptr);
    ::CloseHandle(entry);
    Check(set, "set same-target reparse payload on foreign object");
}

void ForeignReplacement(bool junction) {
    Fixture f;
    SessionManager session;
    StartSession(session, f);
    const auto original = Hash(f.host / L"original.txt");
    Apply(session, f);
    Check(::MoveFileExW(f.host.c_str(), f.parked.c_str(), 0), "park managed junction");
    if (junction) {
        ReplaceWithForeignJunction(f);
    } else {
        fs::create_directory(f.host);
        Write(f.host / L"foreign.txt", "concurrent foreign data");
    }
    const auto sentinel = junction ? f.host / L"data.txt" : f.host / L"foreign.txt";
    const auto before = Hash(sentinel);
    DWORD error = 0;
    Check(!pm::guard::CleanupSession(session, error), "reject foreign replacement");
    Check(session.manifest().session_state() == SessionState::Cleaning &&
          session.manifest().operations()[0].state == OperationState::Failed &&
          Hash(f.backup / L"original.txt") == original && Hash(sentinel) == before,
          "preserve replacement and backup with FAILED pending");
    if (!junction) Check(::DeleteFileW(sentinel.c_str()), "remove fixture replacement file");
    Check(::RemoveDirectoryW(f.host.c_str()) && ::MoveFileExW(f.parked.c_str(), f.host.c_str(), 0),
          "restore managed link to retry");
    Cleanup(session);
    Check(Hash(f.host / L"original.txt") == original, "retry restores exact backup");
    Finalize(session);
}

void InterruptedRename() {
    Fixture f;
    SessionManager session;
    StartSession(session, f);
    const auto original = Hash(f.host / L"original.txt");
    Apply(session, f);
    const auto op_id = session.manifest().operations()[0].id;
    const auto stage = f.root / (L".pm-junction-" + op_id);
    Check(::MoveFileExW(f.host.c_str(), stage.c_str(), 0), "model crash after backup rename");
    Check(session.manifest().UpdateOperationState(op_id, OperationState::IntentRecorded).ok,
          "persist staged operation");
    Cleanup(session);
    Check(::GetFileAttributesW(stage.c_str()) == INVALID_FILE_ATTRIBUTES &&
          Hash(f.host / L"original.txt") == original, "remove staging junction and restore backup");
    Finalize(session);
}

int Owner(const std::wstring& pm, const fs::path& root, const std::wstring& id) {
    SessionManager session;
    Check(session.OpenExistingSession(id).ok, "owner opens session");
    pm::guard::GuardProcess guard;
    DWORD error = 0;
    const bool started = guard.Start(pm, session, error);
    Check(started, "start real PMGuard", error);
    const bool applied = JunctionManager::Prepare(session, pm::core::PathResolver((root / L"portable").native()),
                                                   L"synthetic", (root / L"exclusive").native(), kData, error);
    Check(applied, "owner prepares junction", error);
    const auto guard_pid = std::to_string(::GetProcessId(guard.handle()));
    Write(root / L"guard.txt", guard_pid.c_str());
    const HANDLE ready = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, (L"Local\\PM_Fase04_Ready_" + id).c_str());
    const HANDLE release = ::OpenEventW(SYNCHRONIZE, FALSE, (L"Local\\PM_Fase04_Release_" + id).c_str());
    Check(ready && release && ::SetEvent(ready), "signal owner ready");
    Check(::WaitForSingleObject(release, 30000) == WAIT_OBJECT_0, "wait for release");
    ::CloseHandle(release);
    ::CloseHandle(ready);
    Cleanup(session);
    return 0;
}

struct Close {
    void operator()(void* handle) const { ::CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, Close>;

void WaitForExit(HANDLE process, DWORD expected) {
    DWORD code = 0;
    Check(::WaitForSingleObject(process, 15000) == WAIT_OBJECT_0, "wait for process exit");
    Check(::GetExitCodeProcess(process, &code) && code == expected, "expected process exit", code);
}

void Lifecycle(const std::wstring& self, const std::wstring& pm, bool kill_guard) {
    Fixture f;
    SessionManager session;
    StartSession(session, f);
    const auto original = Hash(f.host / L"original.txt");
    const auto data = Hash(f.target / L"data.txt");
    Handle ready(::CreateEventW(nullptr, TRUE, FALSE, f.ReadyEvent().c_str()));
    Handle release(::CreateEventW(nullptr, TRUE, FALSE, f.ReleaseEvent().c_str()));
    Check(ready && release, "create lifecycle events");
    std::wstring args = L"\"" + self + L"\" --owner \"" + pm + L"\" \"" + f.root.native() + L"\" " + f.id;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    Check(::CreateProcessW(self.c_str(), args.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi), "spawn synthetic PM owner");
    Handle owner(pi.hProcess), owner_thread(pi.hThread);
    try {
        const HANDLE waits[] = {ready.get(), owner.get()};
        Check(::WaitForMultipleObjects(2, waits, FALSE, 15000) == WAIT_OBJECT_0, "owner applies junction");
        DWORD pid = 0;
        std::ifstream(f.root / L"guard.txt") >> pid;
        Handle guard(::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        Check(guard && Hash(f.host / L"data.txt") == data, "guard active with redirect");
        if (kill_guard) {
            Check(::TerminateProcess(guard.get(), 88), "stop synthetic PMGuard");
            WaitForExit(guard.get(), 88);
            Check(Hash(f.host / L"data.txt") == data, "guard death leaves live owner responsible");
            Check(::SetEvent(release.get()), "release owner");
            WaitForExit(owner.get(), 0);
        } else {
            Check(::TerminateProcess(owner.get(), 89), "stop synthetic PM owner");
            WaitForExit(owner.get(), 89);
            WaitForExit(guard.get(), 0);
        }
        Check(session.manifest().LoadFromDisk().ok && session.manifest().session_state() == SessionState::Cleaned,
              "PM or PMGuard persists CLEANED");
        Check(Hash(f.host / L"original.txt") == original && Hash(f.target / L"data.txt") == data,
              "lifecycle preserves both directories");
    } catch (...) {
        ::SetEvent(release.get());
        if (::WaitForSingleObject(owner.get(), 15000) == WAIT_TIMEOUT) {
            ::TerminateProcess(owner.get(), 99);
            ::WaitForSingleObject(owner.get(), 5000);
        }
        Cleanup(session);
        Finalize(session);
        throw;
    }
    ::DeleteFileW((fs::path(session.session_dir()) / L"pm-guard.exe").c_str());
    Finalize(session);
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 5 && std::wstring_view(argv[1]) == L"--owner") {
        try {
            return Owner(argv[2], argv[3], argv[4]);
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
    if (argc != 3) return 2;
    std::ofstream report(fs::path(argv[2]), std::ios::binary);
    if (!report) return 2;
    unsigned passed = 0, failed = 0;
    report << "{\n  \"scenarios\": [\n";
    auto run = [&](const char* name, auto scenario) {
        if (passed + failed) report << ",\n";
        bool ok = false;
        try {
            scenario();
            ok = true;
        } catch (const std::exception& e) {
            std::cerr << name << ": " << e.what() << '\n';
        }
        report << "    {\"name\": \"" << name << "\", \"status\": \""
               << (ok ? "passed" : "failed") << "\"}";
        if (ok) ++passed;
        else ++failed;
    };
    run("original_exists_sha256", [] { RoundTrip(true, false); });
    run("original_absent", [] { RoundTrip(false, false); });
    run("orphaned_target_and_pending_intent", [] { RoundTrip(true, true); });
    run("inaccessible_target_and_existing_backup", Preconditions);
    run("foreign_directory_preserved_and_retry", [] { ForeignReplacement(false); });
    run("same_target_foreign_junction_preserved", [] { ForeignReplacement(true); });
    run("interrupted_after_backup_rename", InterruptedRename);
    const auto self = pm::core::PathResolver::QueryExecutablePath();
    run("pm_crash_guard_restores", [&] { Lifecycle(self, argv[1], false); });
    run("guard_crash_pm_restores", [&] { Lifecycle(self, argv[1], true); });
    report << "\n  ],\n  \"passed\": " << passed << ",\n  \"failed\": " << failed
           << ",\n  \"status\": \"" << (failed ? "failed" : "passed") << "\"\n}\n";
    std::cout << "Fase 4: " << passed << " passed, " << failed << " failed\n";
    return failed || !report ? 1 : 0;
}
