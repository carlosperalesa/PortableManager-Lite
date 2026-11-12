#include "core/config_manager.h"
#include "guard/guard_entry.h"
#include "guard/guard_process.h"
#include "jobs/job_manager.h"
#include "learning/learning_engine.h"
#include "runtime/session_manager.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using pm::core::PathResolver;
using pm::learning::LearningEngine;
using pm::learning::LearningRequest;
using pm::runtime::SessionManager;
namespace fs = std::filesystem;
constexpr std::wstring_view kCompany = L"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
constexpr std::wstring_view kProduct = L"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";

struct CloseHandle {
    void operator()(HANDLE handle) const { ::CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandle>;

void Check(bool ok, const char* step) {
    if (!ok) throw std::runtime_error(std::string(step) + " (Win32 " +
                                      std::to_string(::GetLastError()) + ")");
}

void Write(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    Check(out.good(), "write fixture file");
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void SetKey(const std::wstring& key, DWORD value) {
    HKEY opened = nullptr;
    Check(::RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0,
                            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &opened, nullptr) == ERROR_SUCCESS,
          "create synthetic registry key");
    const auto status = ::RegSetValueExW(opened, L"Marker", 0, REG_DWORD,
                                         reinterpret_cast<const BYTE*>(&value), sizeof(value));
    ::RegCloseKey(opened);
    Check(status == ERROR_SUCCESS, "write synthetic registry value");
}

DWORD KeyMarker(const std::wstring& key) {
    HKEY opened = nullptr;
    const auto open_status = ::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0,
                                            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &opened);
    if (open_status == ERROR_FILE_NOT_FOUND) return 0;
    Check(open_status == ERROR_SUCCESS, "open registry marker");
    DWORD value = 0;
    DWORD size = sizeof(value);
    const auto status = ::RegGetValueW(opened, nullptr, L"Marker", RRF_RT_REG_DWORD,
                                      nullptr, &value, &size);
    ::RegCloseKey(opened);
    Check(status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND, "query registry marker");
    return value;
}

Handle Spawn(const std::wstring& executable, const std::wstring& args) {
    std::wstring command = pm::jobs::QuoteArgument(executable) + L" " + args;
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    PROCESS_INFORMATION process{};
    Check(::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &start, &process) != FALSE,
          "spawn test helper");
    ::CloseHandle(process.hThread);
    return Handle(process.hProcess);
}

DWORD Wait(HANDLE process) {
    Check(::WaitForSingleObject(process, 20000) == WAIT_OBJECT_0, "wait for test helper");
    DWORD code = 0;
    Check(::GetExitCodeProcess(process, &code) != FALSE, "test helper exit code");
    return code;
}

void FixtureExe(const fs::path& path, const std::wstring& company, const std::wstring& product) {
    Check(company.size() == kCompany.size() && product.size() == kProduct.size(), "resource fixture size");
    const auto exe = PathResolver::QueryExecutablePath();
    Check(::CopyFileW(exe.c_str(), path.c_str(), TRUE) != FALSE, "copy synthetic PE");
    const auto version = MAKEINTRESOURCEW(16);
    const auto name = MAKEINTRESOURCEW(1);
    const auto module = ::GetModuleHandleW(nullptr);
    const auto resource = ::FindResourceW(module, name, version);
    Check(resource != nullptr, "find fixture version resource");
    const auto raw = ::LoadResource(module, resource);
    const auto* first = static_cast<const wchar_t*>(::LockResource(raw));
    const DWORD bytes = ::SizeofResource(module, resource);
    Check(first && bytes && bytes % sizeof(wchar_t) == 0, "load version resource");
    std::vector<wchar_t> info(first, first + bytes / sizeof(wchar_t));
    const auto company_at = std::search(info.begin(), info.end(), kCompany.begin(), kCompany.end());
    const auto product_at = std::search(info.begin(), info.end(), kProduct.begin(), kProduct.end());
    Check(company_at != info.end() && product_at != info.end(), "find version strings");
    std::copy(company.begin(), company.end(), company_at);
    std::copy(product.begin(), product.end(), product_at);
    const auto update = ::BeginUpdateResourceW(path.c_str(), FALSE);
    Check(update != nullptr, "open synthetic PE resource for update");
    const bool changed = ::UpdateResourceW(update, version, name, 0x409, info.data(), bytes) != FALSE;
    const bool saved = ::EndUpdateResourceW(update, !changed) != FALSE;
    Check(changed && saved, "save synthetic PE resource");
}

struct Fixture {
    std::wstring company = SessionManager::GenerateSessionGuid();
    std::wstring product = SessionManager::GenerateSessionGuid();
    fs::path root = fs::path(SessionManager::QuerySystemTempDir()) / (L"pm6-" + company);
    fs::path executable = root / L"fixture.exe";
    fs::path roaming = root / L"roaming" / company / product;
    fs::path local = root / L"local" / company / product;
    fs::path portable = root / L"portable";
    std::wstring reg_parent = L"Software\\" + company;
    std::wstring reg = reg_parent + L"\\" + product;
    bool own_root = false;
    bool own_registry = false;

    ~Fixture() {
        if (own_registry) ::RegDeleteTreeW(HKEY_CURRENT_USER, reg_parent.c_str());
        if (own_root) {
            std::error_code error;
            fs::remove_all(root, error);
        }
    }

    void Prepare() {
        Check(!company.empty() && !product.empty() && fs::create_directory(root), "reserve synthetic root");
        own_root = true;
        fs::create_directories(roaming);
        fs::create_directories(local.parent_path());
        fs::create_directories(portable / L"Data" / L"apps" / L"fixture");
        fs::create_directory(root / L"temp");
        Write(roaming / L"original.txt", "host original");
        Write(portable / L"Data" / L"apps" / L"fixture" / L"older.txt", "older data");
        HKEY key = nullptr;
        DWORD disposition = 0;
        const auto opened = ::RegCreateKeyExW(HKEY_CURRENT_USER, reg_parent.c_str(), 0, nullptr, 0,
                                               KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition);
        if (key) ::RegCloseKey(key);
        ::SetLastError(static_cast<DWORD>(opened));
        Check(opened == ERROR_SUCCESS && disposition == REG_CREATED_NEW_KEY, "reserve synthetic registry parent");
        own_registry = true;
        SetKey(reg_parent, 1);
        FixtureExe(executable, company, product);
    }
};

bool Guid(std::wstring_view value) {
    return value.size() == 36 && value.find_first_not_of(L"0123456789abcdef-") == std::wstring_view::npos;
}

void TestEnvironment(const fs::path& root) {
    Check(::SetEnvironmentVariableW(L"APPDATA", (root / L"roaming").c_str()) &&
          ::SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"local").c_str()) &&
          ::SetEnvironmentVariableW(L"TEMP", (root / L"temp").c_str()) &&
          ::SetEnvironmentVariableW(L"TMP", (root / L"temp").c_str()), "set synthetic process environment");
}

LearningRequest Request(const fs::path& root, std::wstring_view mode) {
    const auto exe = (root / L"fixture.exe").wstring();
    const auto candidates = LearningEngine::Inspect(exe);
    Check(candidates && Guid(candidates->company) && Guid(candidates->product) &&
          root.filename().wstring() == L"pm6-" + candidates->company, "bind synthetic executable to root");
    LearningRequest request{exe,
        L"--write " + pm::jobs::QuoteArgument(root.wstring()) + L" " + std::wstring(mode),
        L"fixture", {candidates->roaming, candidates->local, candidates->registry}, 12000};
    if (mode == L"ambiguous") {
        request.exclusive_paths[1].clear();
        request.exclusive_paths[2].clear();
    }
    return request;
}

int Writer(const fs::path& root, std::wstring_view mode) {
    const auto candidates = LearningEngine::Inspect(PathResolver::QueryExecutablePath());
    Check(candidates && Guid(candidates->company) && Guid(candidates->product) &&
          root.filename().wstring() == L"pm6-" + candidates->company, "writer fixture identity");
    const auto roaming = root / L"roaming" / candidates->company / candidates->product;
    const auto local = root / L"local" / candidates->company / candidates->product;
    Check(!fs::exists(roaming / L"original.txt"), "isolated original");
    fs::create_directory(local);
    Write(roaming / L"learned.txt", "roaming output");
    Write(local / L"learned.txt", "local output");
    SetKey(candidates->registry.substr(5), 20);
    Write(root / L"shared.txt", "outside candidates");
    if (mode == L"crash") {
        Write(root / L"ready.txt", "ready");
        ::Sleep(INFINITE);
    }
    if (mode == L"normal") {
        const auto child = Spawn(PathResolver::QueryExecutablePath(), L"--descendant");
        Write(root / L"descendant.txt", std::to_string(::GetProcessId(child.get())));
    }
    return 0;
}

int Controller(const fs::path& root, std::wstring_view mode) {
    auto request = Request(root, mode);
    TestEnvironment(root);
    const PathResolver resolver((root / L"portable").wstring());
    const auto result = LearningEngine::Run(resolver, request);
    if (result.error || result.rollback_error)
        std::fprintf(stderr, "Learning result: error=%lu rollback=%lu\n", result.error, result.rollback_error);
    Check(result.host_restored, "restore host");
    if (mode == L"normal") {
        const auto pid = static_cast<DWORD>(std::stoul(Read(root / L"descendant.txt")));
        const Handle descendant(::OpenProcess(SYNCHRONIZE, FALSE, pid));
        Check(descendant ? ::WaitForSingleObject(descendant.get(), 0) == WAIT_OBJECT_0 :
              ::GetLastError() == ERROR_INVALID_PARAMETER, "drain Job descendants");
    }
    SessionManager session;
    Check(session.OpenExistingSession(result.session_id).ok, "reopen session manifest");
    DWORD error = 0;
    Check(pm::guard::CleanupSession(session, error), "idempotent rollback");
    Check(session.manifest().session_state() == pm::runtime::SessionState::Cleaned, "cleaned manifest");
    const auto data = root / L"portable" / result.data_path;
    Check(Read(data / L"roaming" / L"learned.txt") == "roaming output", "keep learned roaming data");
    if (mode == L"failure") {
        Check(result.error != ERROR_SUCCESS && result.recipe_path.empty(), "report recipe failure");
        Check(fs::exists(data / L"registry.reg"), "retain captured data on recipe failure");
        return 0;
    }
    Check(result.error == ERROR_SUCCESS && !result.recipe_path.empty(), "publish learning recipe");
    const auto recipe = Read(root / L"portable" / result.recipe_path);
    Check(recipe.find("Previous=keep") != std::string::npos, "retain previous recipe");
    Check(recipe.find("RoamingHost=%APPDATA%\\") != std::string::npos &&
          recipe.find("RoamingDataPath=Data/apps/fixture/") != std::string::npos &&
          recipe.find(":\\") == std::string::npos, "portable recipe");
    if (mode == L"ambiguous") {
        Check(recipe.find("LocalHost") == std::string::npos && recipe.find("RegistryKey") == std::string::npos,
              "exclude ambiguous output");
    } else {
        Check(Read(data / L"local" / L"learned.txt") == "local output", "keep learned local data");
        const bool capture_registry = mode != L"existing";
        Check(fs::exists(data / L"registry.reg") == capture_registry &&
              (recipe.find("RegistryDataPath=Data/apps/fixture/") != std::string::npos) == capture_registry,
              "capture only new registry data");
    }
    return 0;
}

void Scenario(std::wstring_view mode) {
    Fixture fixture;
    fixture.Prepare();
    if (mode == L"existing") SetKey(fixture.reg, 7);
    const auto recipes = fixture.portable / L"Data" / L"recipes";
    if (mode == L"failure") Write(recipes, "obstruction");
    else {
        fs::create_directory(recipes);
        Write(recipes / L"fixture.ini", "[Previous]\r\nPrevious=keep\r\n");
    }
    const auto args = L"--case " + pm::jobs::QuoteArgument(fixture.root.wstring()) + L" " + std::wstring(mode);
    auto child = Spawn(PathResolver::QueryExecutablePath(), args);
    if (mode == L"crash") {
        for (unsigned i = 0; i < 500 && !fs::exists(fixture.root / L"ready.txt"); ++i) ::Sleep(20);
        Check(fs::exists(fixture.root / L"ready.txt"), "wait for managed writer");
        Write(fixture.root / L"shared.txt", "concurrent shared output");
        const auto request = Request(fixture.root, mode);
        const auto competing = LearningEngine::Run(PathResolver(fixture.portable.wstring()), request);
        Check(competing.error == ERROR_ALREADY_EXISTS && competing.session_id.empty(), "reserve active candidate");
        Check(::TerminateProcess(child.get(), 42) && Wait(child.get()) == 42, "interrupt learning controller");
        // PMGuard holds the reservation until it exits. Wait without opening
        // session.ini while PMGuard atomically replaces it during rollback.
        const auto reservation = L"Global\\PM_Learning_" + fixture.company + L"/" + fixture.product;
        bool guard_exited = false;
        for (unsigned i = 0; i < 800; ++i) {
            const Handle held(::OpenEventW(SYNCHRONIZE, FALSE, reservation.c_str()));
            if (!held) {
                guard_exited = ::GetLastError() == ERROR_FILE_NOT_FOUND;
                break;
            }
            ::Sleep(20);
        }
        Check(guard_exited, "wait for PMGuard reservation release");
        SessionManager session((fixture.root / L"temp").wstring());
        unsigned sessions = 0;
        for (const auto& entry : fs::directory_iterator(session.base_sessions_root())) {
            if (!entry.is_directory()) continue;
            ++sessions;
            Check(session.OpenExistingSession(entry.path().filename().wstring()).ok, "open crash manifest");
            if (session.manifest().session_state() == pm::runtime::SessionState::Cleaned) continue;
            for (const auto& op : session.manifest().operations()) {
                std::fprintf(stderr, "Crash manifest: operation=%d state=%d error=%lu\n",
                             static_cast<int>(op.type), static_cast<int>(op.state), op.last_error);
            }
            Check(false, "PMGuard completed rollback");
        }
        Check(sessions == 1, "verify the interrupted session");
        Check(Read(recipes / L"fixture.ini") == "[Previous]\r\nPrevious=keep\r\n", "critical exit leaves recipe untouched");
    } else {
        Check(Wait(child.get()) == 0, "learning controller");
    }
    Check(Read(fixture.roaming / L"original.txt") == "host original" &&
          !fs::exists(fixture.roaming / L"learned.txt") &&
          !fs::exists(fixture.roaming.wstring() + L".pmbkp"), "restore original roaming folder");
    if (mode == L"ambiguous") {
        Check(Read(fixture.local / L"learned.txt") == "local output" && KeyMarker(fixture.reg) == 20,
              "leave ambiguous changes in place");
    } else {
        Check(!fs::exists(fixture.local) && KeyMarker(fixture.reg) == (mode == L"existing" ? 7u : 0u),
              "restore local and registry baselines");
    }
    Check(Read(fixture.root / L"shared.txt") == (mode == L"crash" ? "concurrent shared output" : "outside candidates") &&
          KeyMarker(fixture.reg_parent) == 1,
          "preserve shared resources");
    Check(Read(fixture.portable / L"Data" / L"apps" / L"fixture" / L"older.txt") == "older data",
          "preserve existing portable data");
}

void Metadata() {
    Fixture fixture;
    fixture.Prepare();
    const auto candidates = LearningEngine::Inspect(fixture.executable.wstring());
    const auto suffix = L"\\" + fixture.company + L"\\" + fixture.product;
    Check(candidates && candidates->roaming == L"%APPDATA%" + suffix &&
          candidates->local == L"%LOCALAPPDATA%" + suffix &&
          candidates->registry == L"HKCU\\Software" + suffix, "bound PE candidates");
    Check(!LearningEngine::Inspect((fixture.root / L"missing.exe").wstring()), "reject missing PE");
    const auto invalid = fixture.root / L"invalid.exe";
    FixtureExe(invalid, L"../" + std::wstring(33, L'a'), fixture.product);
    Check(!LearningEngine::Inspect(invalid.wstring()), "reject traversal metadata");
    const auto shared = fixture.root / L"shared.exe";
    FixtureExe(shared, L"Microsoft " + std::wstring(26, L'a'), fixture.product);
    Check(!LearningEngine::Inspect(shared.wstring()), "reject shared company");
    auto request = Request(fixture.root, L"normal");
    request.exclusive_paths = {};
    const PathResolver resolver(fixture.portable.wstring());
    Check(LearningEngine::Run(resolver, request).error == ERROR_ACCESS_DENIED, "require independent attribution");
    request.exclusive_paths[0] = L"%TEMP%\\shared";
    Check(LearningEngine::Run(resolver, request).error == ERROR_INVALID_NAME, "reject arbitrary approval");
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 5 && std::wstring_view(argv[1]) == L"--guard")
            return pm::guard::RunGuardMode(argv[2], argv[3], argv[4]);
        if (argc == 4 && std::wstring_view(argv[1]) == L"--case") return Controller(argv[2], argv[3]);
        if (argc == 4 && std::wstring_view(argv[1]) == L"--write") return Writer(argv[2], argv[3]);
        if (argc == 2 && std::wstring_view(argv[1]) == L"--descendant") {
            ::Sleep(INFINITE);
            return 0;
        }
        if (argc != 2) return 2;
        std::ofstream evidence(fs::path(argv[1]), std::ios::binary);
        Check(evidence.good(), "open JSON evidence");
        evidence << "{\n  \"cases\": {\n";
        unsigned failures = 0;
        const auto run = [&](const char* name, const auto& test, bool last = false) {
            bool passed = true;
            try { test(); }
            catch (const std::exception& error) {
                std::fprintf(stderr, "%s: %s\n", name, error.what());
                ++failures;
                passed = false;
            }
            const char* status = passed ? "passed" : "failed";
            std::printf("%s: %s\n", name, status);
            evidence << "    \"" << name << "\": \"" << status << "\"" << (last ? "\n" : ",\n");
        };
        run("pe_metadata_and_scope", Metadata);
        run("learn_restore_and_publish", [] { Scenario(L"normal"); });
        run("preserve_ambiguous_changes", [] { Scenario(L"ambiguous"); });
        run("restore_existing_registry_without_exporting_host_values", [] { Scenario(L"existing"); });
        run("restore_when_recipe_write_fails", [] { Scenario(L"failure"); });
        run("guard_recovers_after_pm_crash", [] { Scenario(L"crash"); }, true);
        evidence << "  },\n  \"status\": \"" << (failures ? "failed" : "passed") << "\"\n}\n";
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
