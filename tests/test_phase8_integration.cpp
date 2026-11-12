#include "core/config_manager.h"
#include "core/path_resolver.h"
#include "runtime/session_manager.h"

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

    void Desktop() const {
        Process process;
        process.Start(pml, L"--desktop", tmp);
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
        const auto send = [&](HWND window, UINT message, WPARAM value) {
            DWORD_PTR reply = 0;
            Check(::SendMessageTimeoutW(window, message, value, 0, SMTO_ABORTIFHUNG, 3000, &reply) != 0,
                  "send PMD navigation command");
        };
        HWND input = nullptr, list = nullptr;
        for (int i = 0; i < 100; ++i) {
            input = ::GetDlgItem(search.hwnd, 1);
            list = ::GetDlgItem(search.hwnd, 2);
            if (input && list && ::IsWindowVisible(search.hwnd)) break;
            ::Sleep(20);
        }
        Check(input && list, "PMD owns search and desktop controls");
        send(search.hwnd, WM_HOTKEY, 1);
        Check(::IsWindowVisible(input), "hotkey opens search");
        send(input, WM_CHAR, L' ');
        Check(!::IsWindowVisible(input) && ::IsWindowVisible(list), "space opens desktop");
        send(list, WM_KEYDOWN, VK_ESCAPE);
        Check(!::IsWindowVisible(list), "escape collapses desktop");
        Check(::PostMessageW(search.hwnd, WM_CLOSE, 0, 0) && process.Wait() == 0, "close PMD normally");
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

int Exercise(const fs::path& root, bool hold) {
    Check(root.filename().native().starts_with(L"pml8-"), "require synthetic root");
    const auto host = PathResolver::ResolveHostTokenPath(kCandidate);
    Check(host.ok && fs::path(host.resolved_path).parent_path().parent_path() == root / L"profile" / L"roaming",
          "limit writes to the isolated profile");
    const fs::path dir(host.resolved_path);
    Check(fs::is_directory(dir), "junction prepared before launch");
    Check(!fs::exists(dir / L"original.txt"), "host original isolated");
    Write(dir / L"written.txt", "synthetic output");
    if (hold) {
        Write(root / L"ready", "ready");
        ::Sleep(INFINITE);
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4 && std::wstring_view(argv[1]) == L"--exercise")
            return Exercise(argv[2], std::wstring_view(argv[3]) == L"hold");
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
        run("reset_archive_reinstall_uninstall", [&] { f.Maintenance(); });
        report << "\n  },\n  \"status\": \"" << (passed ? "passed" : "failed") << "\"\n}\n";
        return passed ? 0 : 1;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "%s\n", ex.what());
        return 1;
    }
}
