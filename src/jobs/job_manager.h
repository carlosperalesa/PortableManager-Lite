#pragma once
#include "runtime/session_manager.h"
#include <windows.h>
#include <string>
#include <string_view>

namespace pm::jobs {
// Hidden preserves a private console; Detached creates no console or conhost.
enum class ConsoleMode { Hidden, Detached };
// One non-inheritable owner handle per application instance. No breakaway allowed.
class JobManager {
public:
    ~JobManager();
    JobManager() = default;
    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;
    static std::wstring MakeName(std::wstring_view session, std::wstring_view instance);
    bool Create(pm::runtime::SessionManager& session, std::wstring_view instance, DWORD& error);
    bool Launch(const std::wstring& executable, std::wstring_view arguments,
                const std::wstring& cwd, PROCESS_INFORMATION& process, DWORD& error,
                ConsoleMode console = ConsoleMode::Hidden);
    static bool TerminateAndWait(const std::wstring& name, DWORD& error);
    const std::wstring& name() const noexcept { return name_; }
private:
    HANDLE job_ = nullptr;
    std::wstring name_;
    std::wstring session_id_, manifest_path_, mutex_name_, operation_id_;
};
// Windows command-line quoting, including trailing backslashes before the closing quote.
std::wstring QuoteArgument(std::wstring_view value);
}
