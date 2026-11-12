#include "core/config_manager.h"
#include "core/path_resolver.h"
#include "runtime/session_manager.h"

#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using pm::runtime::SessionManager;
using pm::core::PathResolver;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string(message) + " (Win32 " + std::to_string(::GetLastError()) + ")");
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\""; // The fixture passes only regular, non-directory paths.
}

struct Process {
    HANDLE handle = nullptr;
    ~Process() {
        if (!handle) return;
        if (::WaitForSingleObject(handle, 0) == WAIT_TIMEOUT) {
            ::TerminateProcess(handle, ERROR_CANCELLED);
            ::WaitForSingleObject(handle, 5000);
        }
        ::CloseHandle(handle);
    }
    void Start(const fs::path& exe, const std::wstring& args, const fs::path& cwd) {
        auto cmd = Quote(exe.native()) + L" " + args;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        Check(::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, cwd.c_str(), &startup, &info) != FALSE, "start integration process");
        ::CloseHandle(info.hThread);
        handle = info.hProcess;
    }
    DWORD Wait() {
        DWORD code = 0;
        Check(::WaitForSingleObject(handle, 30000) == WAIT_OBJECT_0 &&
              ::GetExitCodeProcess(handle, &code), "wait for integration process");
        return code;
    }
};

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void Write(const fs::path& path, const char* content) {
    std::ofstream output(path, std::ios::binary);
    Check(static_cast<bool>(output << content), "write fixture file");
}

template <typename Predicate>
bool Until(Predicate predicate, DWORD timeout = 10000) {
    const auto deadline = ::GetTickCount64() + timeout;
    do {
        if (predicate()) return true;
        ::Sleep(20);
    } while (::GetTickCount64() < deadline);
    return false;
}

struct ChildImage {
    DWORD pid;
    fs::path image;
};

std::vector<ChildImage> Children(DWORD owner, const fs::path& directory, bool guard) {
    std::vector<ChildImage> found;
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ParentProcessID != owner) continue;
        const HANDLE child = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (!child) continue;
        wchar_t image[32768]{};
        DWORD size = static_cast<DWORD>(std::size(image));
        const bool active = ::WaitForSingleObject(child, 0) == WAIT_TIMEOUT &&
                            ::QueryFullProcessImageNameW(child, 0, image, &size);
        ::CloseHandle(child);
        if (!active) continue;
        const fs::path path(image);
        if (guard ? (path.filename() == L"pml.exe" && path.parent_path().parent_path() == directory)
                  : path.parent_path() == directory)
            found.push_back({entry.th32ProcessID, path});
    } while (::Process32NextW(snapshot, &entry));
    ::CloseHandle(snapshot);
    return found;
}

void InterruptGuard(const ChildImage& guard) {
    const auto child = ::OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION |
                                     SYNCHRONIZE, FALSE, guard.pid);
    Check(child != nullptr, "open synthetic guardian");
    wchar_t image[32768]{};
    DWORD size = static_cast<DWORD>(std::size(image));
    const bool stopped = ::QueryFullProcessImageNameW(child, 0, image, &size) &&
                         fs::path(image) == guard.image && ::TerminateProcess(child, 74) &&
                         ::WaitForSingleObject(child, 5000) == WAIT_OBJECT_0;
    ::CloseHandle(child);
    Check(stopped, "interrupt only the verified synthetic guardian");
}

constexpr wchar_t kCandidate[] =
    L"%APPDATA%\\aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\\bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";

struct Fixture {
    fs::path root = fs::path(SessionManager::QuerySystemTempDir()) / (L"pml8-" + SessionManager::GenerateSessionGuid().substr(0, 12));
    fs::path portable = root / L"portable";
    fs::path pml = portable / L"pml.exe";
    fs::path app = portable / L"App" / L"demo";
    fs::path host = root / L"profile" / L"roaming" / L"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa" /
                    L"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
    fs::path tmp = root / L"tmp";
    fs::path package_dir = root / L"package";
    fs::path package = root / L"update.zip";
    bool owned = false;

    ~Fixture() {
        if (owned) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    }

    void Prepare(const fs::path& source) {
        Check(fs::create_directory(root), "reserve unique integration root");
        owned = true;
        fs::create_directories(app);
        fs::create_directories(host);
        fs::create_directory(tmp);
        fs::create_directory(package_dir);
        fs::create_directory(portable / L"Other");
        fs::copy_file(source, pml);
        fs::copy_file(PathResolver::QueryExecutablePath(), app / L"demo.exe");
        Write(host / L"original.txt", "original");
        Write(portable / L"Other" / L"keep.txt", "unrelated document");
        Check(::SetEnvironmentVariableW(L"USERPROFILE", (root / L"profile").c_str()) &&
              ::SetEnvironmentVariableW(L"APPDATA", (root / L"profile" / L"roaming").c_str()) &&
              ::SetEnvironmentVariableW(L"TEMP", tmp.c_str()) && ::SetEnvironmentVariableW(L"TMP", tmp.c_str()),
              "configure synthetic process environment");
    }

    DWORD Run(const std::wstring& args) const {
        Process command;
        command.Start(pml, args, tmp);
        return command.Wait();
    }

    void RegisterAndLearn() const {
        Check(Run(L"--register demo App/demo App/demo/demo.exe Demo") == 0, "register app through CLI");
        Check(Run(L"--list") == 0, "read catalog through CLI");
        Check(Run(L"--learn demo --approve " + Quote(kCandidate) + L" -- --exercise " +
                  Quote(root.native()) + L" once") == 0, "learn through CLI");
        Check(Read(app / L"wrapper.ini").find("RoamingCleanup=REMOVE_JUNCTION_RESTORE_ORIGINAL") != std::string::npos,
              "generate local wrapper");
        Check(Read(host / L"original.txt") == "original" && !fs::exists(host / L"written.txt"), "restore host after learning");
    }

    void ApproveWrapper() const {
        pm::core::ConfigManager config{PathResolver(portable.native())};
        Check(config.LoadAppsIni(false).ok, "load catalog for explicit resource approval");
        std::wstring message;
        Check(config.apps_doc().SetValue(L"App.demo", L"ExclusiveRoaming", kCandidate, message) &&
              config.SaveAppsIniIfModified().ok, "persist explicit resource approval");
    }

    void ZipUpdate() const {
        fs::copy_file(app / L"demo.exe", package_dir / L"demo.exe");
        Write(package_dir / L"WRAPPER.INI", "[Package]\r\nDoNotUse=1\r\n");
        Write(package_dir / L"version.txt", "updated");
        wchar_t system[MAX_PATH]{};
        Check(::GetSystemDirectoryW(system, MAX_PATH) != 0, "find Windows tar");
        Process tar;
        tar.Start(fs::path(system) / L"tar.exe",
                  L"-a -cf " + Quote(package.native()) + L" demo.exe WRAPPER.INI version.txt", package_dir);
        Check(tar.Wait() == 0, "create synthetic ZIP");
        const auto wrapper = Read(app / L"wrapper.ini");
        const auto code = Run(L"--install demo " + Quote(package.native()));
        if (code) ::SetLastError(code);
        Check(code == 0, "update a registered installation");
        Check(Read(app / L"version.txt") == "updated" && Read(app / L"wrapper.ini") == wrapper,
              "update payload and preserve exact wrapper");
    }

    void RunNormally() const {
        Check(Run(L"--run demo --exercise " + Quote(root.native()) + L" once") == 0, "run app under guarded session");
        Check(Read(host / L"original.txt") == "original" && !fs::exists(host / L"written.txt") &&
              Read(portable / L"Data" / L"apps" / L"demo" / L"roaming" / L"written.txt") == "synthetic output",
              "restore exact host original and keep app output on portable storage");
        Check(!fs::exists(tmp / L"pmlog"), "no pmlog on complete normal restoration");
    }

    void RunAndCrash() const {
        fs::remove(root / L"ready");
        Process owner;
        owner.Start(pml, L"--run demo --exercise " + Quote(root.native()) + L" hold", tmp);
        for (int i = 0; i < 500 && !fs::exists(root / L"ready"); ++i) {
            if (::WaitForSingleObject(owner.handle, 20) == WAIT_OBJECT_0) break;
        }
        Check(fs::exists(root / L"ready"), "managed app is running");
        Check(Run(L"--install demo " + Quote(package.native())) != 0, "reject update while running");
        Check(::TerminateProcess(owner.handle, 75) != FALSE && owner.Wait() == 75, "interrupt only the synthetic owner");
        bool restored = false;
        for (int i = 0; i < 750; ++i) {
            if (Read(host / L"original.txt") == "original" && !fs::exists(host / L"written.txt") &&
                Read(tmp / L"pmlog").find("REMOVE_GUARD_SHADOW") != std::string::npos) {
                restored = true;
                break;
            }
            ::Sleep(20);
        }
        Check(restored, "guard restores the host from the local manifest");
        bool guard_exited = false;
        for (int i = 0; i < 250; ++i) {
            const auto backup = host.native() + L".pmbkp";
            if (!fs::exists(backup) && Run(L"--run demo --exercise " + Quote(root.native()) + L" once") == 0) {
                guard_exited = true;
                break;
            }
            ::Sleep(30);
        }
        Check(guard_exited, "guard released application reservation");
    }

    HWND DesktopWindow(Process& process) const {
        struct WindowSearch { DWORD pid; HWND hwnd; } search{::GetProcessId(process.handle), nullptr};
        const auto callback = [](HWND window, LPARAM value) -> BOOL {
            auto& found = *reinterpret_cast<WindowSearch*>(value);
            DWORD pid = 0;
            ::GetWindowThreadProcessId(window, &pid);
            wchar_t name[80]{};
            ::GetClassNameW(window, name, static_cast<int>(std::size(name)));
            if (pid != found.pid || std::wstring_view(name) != L"PML_PortableManagerDesktop") return TRUE;
            found.hwnd = window;
            return FALSE;
        };
        for (int i = 0; i < 200 && !search.hwnd; ++i) {
            ::EnumWindows(callback, reinterpret_cast<LPARAM>(&search));
            if (!search.hwnd) ::Sleep(20);
        }
        Check(search.hwnd != nullptr, "PMD owns a native pill window");
        Check(Until([&] { return ::GetDlgItem(search.hwnd, 1) && ::GetDlgItem(search.hwnd, 2) &&
                                 ::IsWindowVisible(search.hwnd); }), "PMD controls are ready");
        return search.hwnd;
    }

    static void Send(HWND window, UINT message, WPARAM value, LPARAM data = 0) {
        DWORD_PTR reply = 0;
        Check(::SendMessageTimeoutW(window, message, value, data, SMTO_ABORTIFHUNG, 3000, &reply) != 0,
              "send PMD navigation command");
    }

    void Desktop() const {
        Process process;
        process.Start(pml, L"--desktop", tmp);
        const auto window = DesktopWindow(process);
        const auto input = ::GetDlgItem(window, 1);
        const auto list = ::GetDlgItem(window, 2);
        Send(window, WM_HOTKEY, 1);
        Check(::IsWindowVisible(input), "hotkey opens search");
        Send(input, WM_CHAR, L' ');
        Check(!::IsWindowVisible(input) && ::IsWindowVisible(list), "space opens desktop");
        Send(list, WM_KEYDOWN, VK_ESCAPE);
        Check(!::IsWindowVisible(list), "escape collapses desktop");
        Check(::PostMessageW(window, WM_CLOSE, 0, 0) && process.Wait() == 0, "close PMD normally");
    }

    ChildImage OneGuard(const Process& owner, DWORD previous = 0) const {
        const auto sessions = tmp / L"PortableManager" / L"Session";
        const DWORD pid = ::GetProcessId(owner.handle);
        std::vector<ChildImage> children;
        Check(Until([&] {
            children = Children(pid, sessions, true);
            return children.size() == 1 && children.front().pid != previous;
        }, 15000), "one guardian for the PML process");
        return children.front();
    }

    void SharedSession() const {
        const auto second = app.parent_path() / L"second";
        fs::create_directory(second);
        fs::copy_file(PathResolver::QueryExecutablePath(), second / L"second.exe");
        Check(Run(L"--register second App/second App/second/second.exe Second") == 0, "register second app");
        Process owner;
        owner.Start(pml, L"--desktop", tmp);
        const auto window = DesktopWindow(owner);
        const auto guardian = OneGuard(owner);
        const auto manifest = guardian.image.parent_path() / L"session.ini";
        Check(Read(manifest).find("KILL_JOB") == std::string::npos, "guardian and manifest start before the first Job");

        const auto release = root / L"release-first";
        const auto launch = [&](const std::wstring& command) {
            Send(window, WM_HOTKEY, 1);
            const auto input = ::GetDlgItem(window, 1);
            Send(input, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(command.c_str()));
            Send(input, WM_KEYDOWN, VK_RETURN);
        };
        launch(L"demo --exercise " + Quote(root.native()) + L" until-release");
        const auto owner_pid = ::GetProcessId(owner.handle);
        Check(Until([&] { return Read(host / L"written.txt") == "synthetic output" &&
                                 !fs::exists(host / L"original.txt"); }), "first application is active");
        launch(L"second --idle " + Quote(root.native()));
        Check(Until([&] { return fs::exists(root / L"ready-second"); }), "second application is active");
        Check(Children(owner_pid, app, false).size() == 1 && Children(owner_pid, second, false).size() == 1 &&
              OneGuard(owner).pid == guardian.pid, "two Jobs and still one guardian");
        const auto active = Read(manifest);
        Check(active.find("AppId=demo") != std::string::npos && active.find("AppId=second") != std::string::npos,
              "shared manifest records both applications");

        InterruptGuard(guardian);
        const auto replacement = OneGuard(owner, guardian.pid);
        Check(replacement.image == guardian.image &&
              Children(owner_pid, app, false).size() == 1 && Children(owner_pid, second, false).size() == 1,
              "replace guardian in the same session while both apps keep running");
        Check(Run(L"--install demo " + Quote(package.native())) != 0, "replacement keeps application reservation");
        Write(release, "release");
        Check(Until([&] {
            const auto current = Read(manifest);
            return Read(host / L"original.txt") == "original" &&
                   Children(owner_pid, app, false).empty() &&
                   current.find("AppId=second") != std::string::npos &&
                   current.find("AppId=demo") == std::string::npos;
        }), "closing first app restores only its resources and retires its entries");
        Check(Children(owner_pid, second, false).size() == 1 && OneGuard(owner).pid == replacement.pid,
              "unit close leaves the other app and the guardian alive");
        const auto remaining = Children(owner_pid, second, false);
        const auto child = ::OpenProcess(SYNCHRONIZE, FALSE, remaining.front().pid);
        Check(child != nullptr, "observe remaining synthetic app");
        const auto guard = ::OpenProcess(SYNCHRONIZE, FALSE, replacement.pid);
        Check(guard != nullptr, "observe replacement guardian");
        const bool interrupted = ::TerminateProcess(owner.handle, 76) && owner.Wait() == 76;
        const bool child_exited = ::WaitForSingleObject(child, 10000) == WAIT_OBJECT_0;
        const bool guard_exited = ::WaitForSingleObject(guard, 10000) == WAIT_OBJECT_0;
        ::CloseHandle(child);
        ::CloseHandle(guard);
        Check(interrupted && child_exited && guard_exited &&
              Read(manifest).find("State=CLEANED") != std::string::npos,
              "replacement restores the full shared session after owner crash");
    }

    void RetryBudget() const {
        Process owner;
        owner.Start(pml, L"--desktop", tmp);
        const auto window = DesktopWindow(owner);
        auto guardian = OneGuard(owner);
        const auto session_dir = guardian.image.parent_path();
        for (unsigned i = 0; i < 3; ++i) {
            InterruptGuard(guardian);
            guardian = OneGuard(owner, guardian.pid);
            Check(guardian.image.parent_path() == session_dir, "retry uses the same session");
        }
        InterruptGuard(guardian);
        const auto sessions = tmp / L"PortableManager" / L"Session";
        const auto owner_pid = ::GetProcessId(owner.handle);
        Check(!Until([&] { return !Children(owner_pid, sessions, true).empty(); }, 5500),
              "retry budget remains exhausted");
        Send(window, WM_HOTKEY, 1);
        Check(Children(owner_pid, sessions, true).empty(), "opening PMD does not reset retry budget");
        Check(::PostMessageW(window, WM_CLOSE, 0, 0) && owner.Wait() == 0, "PML can cleanly close without PMG");
        Check(!fs::exists(session_dir), "remove the retired session after exhausted retries");
    }

    void Maintenance() const {
        const auto wrapper = Read(app / L"wrapper.ini");
        Check(Run(L"--reset demo --confirm") == 0 && !fs::exists(portable / L"Data" / L"apps" / L"demo") &&
              Read(app / L"wrapper.ini") == wrapper, "reset only portable application data");
        Check(Run(L"--archive demo --confirm") == 0 && !fs::exists(app) &&
              Read(portable / L"Data" / L"archived" / L"demo" / L"wrapper.ini") == wrapper, "archive retains wrapper");
        Check(Run(L"--install demo " + Quote(package.native())) == 0 &&
              Read(app / L"wrapper.ini") == wrapper, "reinstall restores archive wrapper");
        Check(Run(L"--uninstall demo --confirm") == 0 && !fs::exists(app) &&
              Read(portable / L"Other" / L"keep.txt") == "unrelated document", "uninstall protects Other");
    }
};

int Exercise(const fs::path& root, std::wstring_view mode) {
    Check(root.filename().native().starts_with(L"pml8-"), "require synthetic root");
    const auto host = PathResolver::ResolveHostTokenPath(kCandidate);
    Check(host.ok && fs::path(host.resolved_path).parent_path().parent_path() == root / L"profile" / L"roaming",
          "limit writes to the isolated profile");
    const fs::path dir(host.resolved_path);
    Check(fs::is_directory(dir), "junction prepared before launch");
    Check(!fs::exists(dir / L"original.txt"), "host original isolated");
    Write(dir / L"written.txt", "synthetic output");
    if (mode == L"hold") {
        Write(root / L"ready", "ready");
        ::Sleep(INFINITE);
    }
    if (mode == L"until-release")
        Check(Until([&] { return fs::exists(root / L"release-first"); }, 60000), "wait for synthetic app release");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4 && std::wstring_view(argv[1]) == L"--exercise")
            return Exercise(argv[2], argv[3]);
        if (argc == 3 && std::wstring_view(argv[1]) == L"--idle") {
            const fs::path root(argv[2]);
            Check(root.filename().native().starts_with(L"pml8-"), "require synthetic root");
            Write(root / L"ready-second", "ready");
            ::Sleep(INFINITE);
            return 0;
        }
        if (argc != 3) return 2;
        std::ofstream report(fs::path(argv[2]), std::ios::binary);
        Check(report.good(), "open integration report");
        Fixture f;
        f.Prepare(argv[1]);
        report << "{\n  \"cases\": {\n";
        unsigned cases = 0;
        bool passed = true;
        const auto run = [&](const char* name, const auto& test) {
            bool ok = false;
            if (passed) {
                try { test(); ok = true; }
                catch (const std::exception& ex) { std::fprintf(stderr, "%s: %s\n", name, ex.what()); }
            }
            const auto status = ok ? "passed" : passed ? "failed" : "not_run";
            report << (cases++ ? ",\n" : "") << "    \"" << name << "\": \"" << status << "\"";
            std::printf("%s: %s\n", name, status);
            passed = passed && ok;
        };
        run("register_and_learn", [&] { f.RegisterAndLearn(); });
        run("approved_wrapper_normal_launch", [&] { f.ApproveWrapper(); f.RunNormally(); });
        run("update_preserves_wrapper", [&] { f.ZipUpdate(); });
        run("guard_rollback_and_update_exclusion", [&] { f.RunAndCrash(); });
        run("pmd_hotkey_space_and_escape", [&] { f.Desktop(); });
        run("single_guard_shared_manifest_and_replacement", [&] { f.SharedSession(); });
        run("guard_retry_budget", [&] { f.RetryBudget(); });
        run("reset_archive_reinstall_uninstall", [&] { f.Maintenance(); });
        report << "\n  },\n  \"status\": \"" << (passed ? "passed" : "failed") << "\"\n}\n";
        return passed ? 0 : 1;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "%s\n", ex.what());
        return 1;
    }
}
