#include "applications/app_paths.h"
#include "applications/session_host.h"
#include "core/config_manager.h"
#include "guard/guard_process.h"
#include "jobs/job_manager.h"
#include "junctions/junction_manager.h"
#include "learning/learning_engine.h"
#include "registry/registry_manager.h"
#include "runtime/session_lock.h"

#include <cwchar>
#include <map>

namespace pm::applications {
namespace {
using core::ConfigManager;
using core::IniDocument;
using core::PathResolver;

constexpr wchar_t kCatalogLock[] = L"Global\\PML_Catalog";

bool Missing(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

App ReadApp(const IniDocument& doc, const std::wstring& section) {
    const auto get = [&](const wchar_t* key) { return doc.GetValue(section, key); };
    App app;
    app.id = section.substr(4);
    app.name = get(L"Name");
    if (app.name.empty()) app.name = app.id;
    app.directory = get(L"InstallDir");
    app.executable = get(L"Executable");
    if (app.executable.empty()) app.executable = get(L"Launcher");
    app.aliases = get(L"Aliases");
    app.extensions = get(L"Extensions");
    app.arguments = get(L"Arguments");
    app.color = get(L"Color");
    app.environment = get(L"Environment");
    app.exclusive_roaming = get(L"ExclusiveRoaming");
    app.exclusive_local = get(L"ExclusiveLocal");
    app.exclusive_registry = get(L"ExclusiveRegistry");
    app.paf = Equal(get(L"Type"), L"PAF");
    app.archived = Equal(get(L"State"), L"Archived");
    app.browser = Equal(get(L"IsDefaultBrowser"), L"true");
    return app;
}

DWORD LoadCatalog(const PathResolver& resolver, IniDocument& doc) {
    const auto path = resolver.ResolvePmRelativePath(L"Data\\apps.ini");
    if (!path.ok) return ERROR_INVALID_NAME;
    Pins pins;
    const DWORD pinned = PinDirectory(path.resolved_path.substr(0, path.resolved_path.rfind(L'\\')), pins);
    if (pinned) return Missing(pinned) ? ERROR_SUCCESS : pinned;
    const auto file = core::AtomicFile::ReadUtf8Text(path.resolved_path, true);
    if (!file.ok) return Missing(file.win32_error) ? ERROR_SUCCESS : file.win32_error;
    std::wstring error;
    return doc.ParseUtf8(file.content, error) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

bool AppSection(const IniDocument::EntryLine& line, const IniDocument& doc) {
    return line.kind == IniDocument::LineKind::Section && line.section_name.size() > 4 &&
           Equal(line.section_name.substr(0, 4), L"App.") &&
           !Equal(doc.GetValue(line.section_name, L"State"), L"Removed");
}

struct Policy {
    std::wstring roaming;
    std::wstring local;
    std::wstring registry;
    Handle lease;
};

DWORD LoadPolicy(const PathResolver& resolver, const App& app, const std::wstring& exe, Policy& policy) {
    if (app.paf || (app.exclusive_roaming.empty() && app.exclusive_local.empty() && app.exclusive_registry.empty()))
        return ERROR_SUCCESS;
    if (!app.environment.empty()) return ERROR_INVALID_PARAMETER;
    const auto candidates = learning::LearningEngine::Inspect(exe);
    if (!candidates) return ERROR_INVALID_DATA;
    const auto path = resolver.ResolvePmRelativePath(app.directory + L"\\wrapper.ini");
    const auto wrapper = core::AtomicFile::ReadUtf8Text(path.resolved_path, true);
    if (!wrapper.ok) return wrapper.win32_error;
    IniDocument doc;
    std::wstring message;
    if (!doc.ParseUtf8(wrapper.content, message)) return ERROR_INVALID_DATA;
    const auto latest = doc.GetValue(L"Learning", L"LatestSession");
    if (!runtime::SessionManager::IsValidSessionId(latest)) return ERROR_INVALID_DATA;
    const auto section = L"Learning." + latest;
    const auto allowed = [&](const std::wstring& approval, const std::wstring& candidate,
                             const wchar_t* key, const wchar_t* rule, const wchar_t* expected) {
        return approval.empty() || (approval == candidate && approval == doc.GetValue(section, key) &&
                                    doc.GetValue(section, rule) == expected);
    };
    if (!allowed(app.exclusive_roaming, candidates->roaming, L"RoamingHost", L"RoamingCleanup", L"REMOVE_JUNCTION_RESTORE_ORIGINAL") ||
        !allowed(app.exclusive_local, candidates->local, L"LocalHost", L"LocalCleanup", L"REMOVE_JUNCTION_RESTORE_ORIGINAL") ||
        !allowed(app.exclusive_registry, candidates->registry, L"RegistryKey", L"RegistryCleanup", L"RESTORE_REG"))
        return ERROR_ACCESS_DENIED;
    policy.roaming = app.exclusive_roaming;
    policy.local = app.exclusive_local;
    policy.registry = app.exclusive_registry;
    auto resource = candidates->company + L"/" + candidates->product;
    for (auto& ch : resource)
        if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
    const auto lease_name = L"Global\\PM_Learning_" + resource;
    ::SetLastError(ERROR_SUCCESS);
    policy.lease.reset(::CreateEventW(nullptr, TRUE, FALSE, lease_name.c_str()));
    const DWORD error = ::GetLastError();
    return !policy.lease || error == ERROR_ALREADY_EXISTS ? error : ERROR_SUCCESS;
}

DWORD PreparePolicy(runtime::SessionManager& session, const PathResolver& resolver,
                    const App& app, const Policy& policy) {
    const std::pair<const std::wstring&, const wchar_t*> folders[] = {
        {policy.roaming, L"roaming"}, {policy.local, L"local"}
    };
    for (const auto& [token, name] : folders) {
        if (token.empty()) continue;
        const auto host = PathResolver::ResolveHostTokenPath(token);
        if (!host.ok) return ERROR_INVALID_NAME;
        const auto data = L"Data\\apps\\" + app.id + L"\\" + name;
        Pins pins;
        DWORD error = EnsureDirectory(resolver, data, pins);
        if (error) return error;
        if (!junctions::JunctionManager::Prepare(session, resolver, app.id, host.resolved_path, data, error))
            return error;
    }
    DWORD error = ERROR_SUCCESS;
    if (!policy.registry.empty() && !registry::RegistryManager::Prepare(session, policy.registry, error, true, app.id))
        return error;
    return ERROR_SUCCESS;
}

struct EnvLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};

DWORD MakeEnvironment(const PathResolver& resolver, const App& app, std::wstring& block) {
    if (app.environment.empty() || app.paf) return ERROR_SUCCESS;
    std::map<std::wstring, std::wstring, EnvLess> values;
    const auto original = ::GetEnvironmentStringsW();
    if (!original) return ::GetLastError();
    for (auto entry = original; *entry; entry += std::wcslen(entry) + 1) {
        std::wstring value(entry);
        const auto split = value.find(L'=', 1);
        if (split != std::wstring::npos) values[value.substr(0, split)] = value.substr(split + 1);
    }
    ::FreeEnvironmentStringsW(original);
    for (std::size_t start = 0; start < app.environment.size();) {
        const auto end = app.environment.find(L',', start);
        const auto name = app.environment.substr(start, end - start);
        if (name != L"APPDATA" && name != L"LOCALAPPDATA" && name != L"USERPROFILE")
            return ERROR_INVALID_PARAMETER;
        const auto relative = L"Data\\apps\\" + app.id + L"\\environment\\" + name;
        Pins pins;
        const DWORD error = EnsureDirectory(resolver, relative, pins);
        if (error) return error;
        values[name] = resolver.ResolvePmRelativePath(relative).resolved_path;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    for (const auto& [name, value] : values) {
        block += name + L"=" + value;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return ERROR_SUCCESS;
}

} // namespace

DWORD List(const PathResolver& resolver, std::vector<App>& apps) {
    runtime::SessionLock lock(kCatalogLock);
    if (!lock.ok()) return lock.error();
    IniDocument doc;
    const DWORD error = LoadCatalog(resolver, doc);
    if (error) return error;
    apps.clear();
    for (const auto& line : doc.lines()) {
        if (!AppSection(line, doc)) continue;
        auto app = ReadApp(doc, line.section_name);
        if (ValidateApp(resolver, app, false) == ERROR_SUCCESS) apps.push_back(std::move(app));
    }
    return ERROR_SUCCESS;
}

DWORD SaveApp(const PathResolver& resolver, App app, bool removed) {
    const DWORD validated = ValidateApp(resolver, app, !removed);
    if (validated) return validated;
    runtime::SessionLock lock(kCatalogLock);
    if (!lock.ok()) return lock.error();
    Pins pins;
    DWORD error = EnsureDirectory(resolver, L"Data", pins);
    if (error) return error;
    ConfigManager config(resolver);
    auto& doc = config.apps_doc();
    error = LoadCatalog(resolver, doc);
    if (error) return error;
    const auto section = L"App." + app.id;
    for (const auto& line : doc.lines()) {
        if (!AppSection(line, doc)) continue;
        auto other = ReadApp(doc, line.section_name);
        if (ValidateApp(resolver, other, false) != ERROR_SUCCESS) continue;
        const bool same_id = Equal(section, line.section_name);
        const bool same_directory = Equal(other.directory, app.directory);
        if (same_id != same_directory) return ERROR_ALREADY_EXISTS;
    }
    std::wstring message;
    if (removed) {
        std::vector<std::wstring> associations;
        for (const auto& line : doc.lines())
            if (line.kind == IniDocument::LineKind::KeyValue && Equal(line.section_name, L"Associations") &&
                Equal(line.value, app.id)) associations.push_back(line.key);
        for (const auto& key : associations) doc.RemoveKey(L"Associations", key);
        doc.RemoveSection(section);
    } else {
        const std::pair<const wchar_t*, std::wstring_view> fields[] = {
            {L"Name", app.name}, {L"Aliases", app.aliases}, {L"Extensions", app.extensions},
            {L"Arguments", app.arguments}, {L"Color", app.color}, {L"Environment", app.environment},
            {L"ExclusiveRoaming", app.exclusive_roaming}, {L"ExclusiveLocal", app.exclusive_local},
            {L"ExclusiveRegistry", app.exclusive_registry}, {L"Type", app.paf ? L"PAF" : L"Archive"},
            {L"State", app.archived ? L"Archived" : L"Installed"},
            {L"IsDefaultBrowser", app.browser ? L"true" : L"false"}
        };
        for (const auto& [key, value] : fields)
            if (!doc.SetValue(section, key, value, message)) return ERROR_INVALID_DATA;
        if (!doc.SetRelativePathValue(section, L"InstallDir", app.directory, message) ||
            !doc.SetRelativePathValue(section, L"Executable", app.executable, message))
            return ERROR_INVALID_DATA;
    }
    return config.SaveAppsIniIfModified().win32_error;
}

DWORD Register(const PathResolver& resolver, const App& app) {
    auto normalized = app;
    const DWORD error = ValidateApp(resolver, normalized, true);
    if (error) return error;
    Handle lease;
    const DWORD acquired = AcquireApp(resolver, normalized.directory, lease);
    return acquired ? acquired : SaveApp(resolver, normalized, false);
}

DWORD Association(const PathResolver& resolver, std::wstring_view extension, std::wstring& id) {
    runtime::SessionLock lock(kCatalogLock);
    if (!lock.ok()) return lock.error();
    IniDocument doc;
    const DWORD error = LoadCatalog(resolver, doc);
    if (!error) id = doc.GetValue(L"Associations", extension);
    return error;
}

DWORD SetAssociation(const PathResolver& resolver, std::wstring_view extension, std::wstring_view id) {
    if (!SafeId(id) || extension.empty()) return ERROR_INVALID_PARAMETER;
    runtime::SessionLock lock(kCatalogLock);
    if (!lock.ok()) return lock.error();
    ConfigManager config(resolver);
    auto& doc = config.apps_doc();
    DWORD error = LoadCatalog(resolver, doc);
    if (error) return error;
    std::wstring message;
    if (!doc.SetValue(L"Associations", extension, id, message)) return ERROR_INVALID_NAME;
    Pins pins;
    error = EnsureDirectory(resolver, L"Data", pins);
    return error ? error : config.SaveAppsIniIfModified().win32_error;
}

DWORD Find(const PathResolver& resolver, std::wstring_view id, App& app) {
    std::vector<App> apps;
    const DWORD error = List(resolver, apps);
    if (error) return error;
    for (const auto& candidate : apps) {
        if (Equal(candidate.id, id)) {
            app = candidate;
            return ERROR_SUCCESS;
        }
    }
    return ERROR_NOT_FOUND;
}

RunResult Run(const PathResolver& resolver, const App& selected, std::wstring_view arguments,
              const StopRequest& stop, SessionHost& host) {
    RunResult result;
    App app = selected;
    const auto stop_signaled = [&] { return stop.event && ::WaitForSingleObject(stop.event, 0) == WAIT_OBJECT_0; };
    if (stop_signaled() || host.closing()) return result;
    if (app.archived) return {ERROR_FILE_NOT_FOUND};
    result.error = ValidateApp(resolver, app, false);
    if (result.error) return result;
    Handle lease;
    result.error = AcquireApp(resolver, app.directory, lease);
    if (result.error) return result;
    const auto& origin = host.origin();
    const auto executable = resolver.ResolvePmRelativePath(app.executable).resolved_path;
    const auto cwd = executable.substr(0, executable.rfind(L'\\'));
    if (!guard::IsOriginAccessible(origin)) return {ERROR_DEVICE_NOT_CONNECTED};
    Pins pins;
    Handle file;
    result.error = PinDirectory(cwd, pins);
    if (result.error) return result;
    result.error = CheckFile(executable, file);
    if (result.error) return result;
    Policy policy;
    result.error = LoadPolicy(resolver, app, executable, policy);
    if (result.error) return result;
    std::wstring environment;
    result.error = MakeEnvironment(resolver, app, environment);
    if (result.error) return result;

    auto& session = host.session();
    auto job = std::make_shared<jobs::JobManager>();
    bool added = false;
    const auto launch = [&]() -> DWORD {
        DWORD error = ERROR_SUCCESS;
        PROCESS_INFORMATION process{};
        {
            runtime::SessionLock lock(session.session_mutex_name());
            if (!lock.ok()) return lock.error();
            if (stop_signaled() || host.closing()) return ERROR_SUCCESS;
            const HANDLE reservations[] = {lease.get(), policy.lease.get()};
            error = host.Add(app.id, job, reservations, policy.lease != nullptr);
            if (error) return error == ERROR_CANCELLED ? ERROR_SUCCESS : error;
            added = true;
            error = PreparePolicy(session, resolver, app, policy);
            if (error) return error;
            if (!job->Create(session, app.id, error)) return error;
            if (stop_signaled() || host.closing()) return ERROR_SUCCESS;
            auto command = app.arguments;
            if (!arguments.empty()) {
                if (!command.empty()) command += L" ";
                command += arguments;
            }
            if (!job->Launch(executable, command, cwd, process, error, jobs::ConsoleMode::Detached,
                             environment.empty() ? nullptr : environment.c_str())) return error;
        }
        file.reset();
        pins.clear();
        ::CloseHandle(process.hThread);
        const Handle child(process.hProcess);
        auto wait = guard::WaitForReleaseOrOriginLoss(origin, child.get(), INFINITE, error, stop.event);
        if (wait == guard::OriginWaitResult::Released) {
            if (!::GetExitCodeProcess(child.get(), &result.exit_code)) return ::GetLastError();
            while (!jobs::JobManager::IsEmpty(job->name())) {
                wait = guard::WaitForReleaseOrOriginLoss(origin, nullptr, 200, error, stop.event);
                if (wait != guard::OriginWaitResult::TimedOut) break;
            }
        }
        if (wait == guard::OriginWaitResult::OriginLost) return ERROR_DEVICE_NOT_CONNECTED;
        return wait == guard::OriginWaitResult::Failed ? error : ERROR_SUCCESS;
    };
    try {
        result.error = launch();
    } catch (...) {
        result.error = ERROR_UNHANDLED_EXCEPTION;
    }
    if (!added) return result;
    if (result.error == ERROR_DEVICE_NOT_CONNECTED) host.BeginShutdown();
    runtime::SessionLock lock(session.session_mutex_name());
    if (!lock.ok()) {
        result.rollback_error = lock.error();
        return result;
    }
    if (host.closing()) return result; // The owner closes all Jobs together.
    const auto mode = result.error ? cleanup::Mode::Critical : stop.mode.load();
    const auto cleaned = cleanup::CleanupEngine::RunApplication(session, app.id, mode);
    result.rollback_error = cleaned.error;
    if (cleaned.ok) {
        job.reset();
        host.Release(app.id);
    }
    return result;
}

} // namespace pm::applications
