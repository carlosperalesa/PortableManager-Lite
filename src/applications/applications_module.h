#pragma once

#include "cleanup/cleanup_module.h"
#include "core/path_resolver.h"

#include <atomic>
#include <string>
#include <vector>

namespace pm::applications {

struct App {
    std::wstring id;
    std::wstring name;
    std::wstring directory;
    std::wstring executable;
    std::wstring aliases;
    std::wstring extensions;
    std::wstring arguments;
    std::wstring color;
    std::wstring environment;
    std::wstring exclusive_roaming;
    std::wstring exclusive_local;
    std::wstring exclusive_registry;
    bool paf = false;
    bool archived = false;
    bool browser = false;
    int learn_runs = 0;
};

class SessionHost;

struct StopRequest {
    HANDLE event = nullptr;
    std::atomic<cleanup::Mode> mode{cleanup::Mode::Normal};
};

struct RunResult {
    DWORD error = ERROR_SUCCESS;
    DWORD rollback_error = ERROR_SUCCESS;
    DWORD exit_code = 0;
};

DWORD List(const core::PathResolver& resolver, std::vector<App>& apps);
DWORD Register(const core::PathResolver& resolver, const App& app);
DWORD Association(const core::PathResolver& resolver, std::wstring_view extension, std::wstring& id);
DWORD SetAssociation(const core::PathResolver& resolver, std::wstring_view extension, std::wstring_view id);
DWORD Find(const core::PathResolver& resolver, std::wstring_view id, App& app);
RunResult Run(const core::PathResolver& resolver, const App& app, std::wstring_view arguments,
              const StopRequest& stop, SessionHost& host);
DWORD Install(const core::PathResolver& resolver, std::wstring_view id,
              const std::wstring& package, std::wstring_view selected_executable,
              std::vector<std::wstring>* executable_choices = nullptr);
enum class Maintenance { Reset, Archive, Uninstall };
DWORD Maintain(const core::PathResolver& resolver, const App& app, Maintenance action);

} // namespace pm::applications
