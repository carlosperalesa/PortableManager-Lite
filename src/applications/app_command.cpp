#include "applications/app_command.h"
#include "applications/app_paths.h"
#include "jobs/job_manager.h"
#include "learning/learning_engine.h"

#include <cstdio>

namespace pm::applications {
namespace {
std::atomic<StopRequest*> active_stop{nullptr};

BOOL WINAPI StopConsole(DWORD signal) {
    if (signal != CTRL_C_EVENT && signal != CTRL_BREAK_EVENT) return FALSE;
    auto* stop = active_stop.load();
    if (!stop) return FALSE;
    stop->mode.store(cleanup::Mode::Fast);
    return ::SetEvent(stop->event);
}

std::wstring Arguments(int start, int argc, wchar_t** argv) {
    std::wstring result;
    for (int i = start; i < argc; ++i) {
        if (!result.empty()) result += L" ";
        result += jobs::QuoteArgument(argv[i]);
    }
    return result;
}

int Status(DWORD error) {
    if (error) std::fwprintf(stderr, L"[PML] Error Win32 %lu.\n", error);
    return error ? 1 : 0;
}

} // namespace

int RunCommand(const core::PathResolver& resolver, std::wstring_view command, int argc, wchar_t** argv) {
    if (command == L"--list" && argc == 2) {
        std::vector<App> apps;
        const DWORD error = List(resolver, apps);
        if (error) return Status(error);
        for (const auto& app : apps)
            std::wprintf(L"%ls\t%ls%ls\n", app.id.c_str(), app.name.c_str(), app.archived ? L" (archivado)" : L"");
        return 0;
    }
    if (command == L"--register" && (argc == 5 || argc == 6)) {
        App app;
        app.id = argv[2];
        app.name = argc == 6 ? argv[5] : argv[2];
        app.directory = argv[3];
        app.executable = argv[4];
        App old;
        if (!Find(resolver, app.id, old)) return Status(ERROR_ALREADY_EXISTS);
        return Status(Register(resolver, app));
    }
    if (command == L"--install" && (argc == 4 || argc == 5)) {
        std::vector<std::wstring> choices;
        const auto error = Install(resolver, argv[2], argv[3], argc == 5 ? argv[4] : L"", &choices);
        if (error == ERROR_MORE_DATA)
            for (const auto& choice : choices) std::fwprintf(stderr, L"%ls\\n", choice.c_str());
        return Status(error);
    }
    if ((command == L"--reset" || command == L"--archive" || command == L"--uninstall") &&
        argc == 4 && std::wstring_view(argv[3]) == L"--confirm") {
        App app;
        const DWORD error = Find(resolver, argv[2], app);
        if (error) return Status(error);
        const auto action = command == L"--reset" ? Maintenance::Reset :
                            command == L"--archive" ? Maintenance::Archive : Maintenance::Uninstall;
        return Status(Maintain(resolver, app, action));
    }
    if ((command == L"--run" || command == L"--learn") && argc >= 3) {
        App app;
        const DWORD error = Find(resolver, argv[2], app);
        if (error) return Status(error);
        const auto temp = runtime::SessionManager::QuerySystemTempDir();
        if (temp.empty() || !::SetCurrentDirectoryW(temp.c_str())) return Status(::GetLastError());
        if (command == L"--learn") {
            if (app.archived || app.paf || argc < 5 || std::wstring_view(argv[3]) != L"--approve")
                return Status(ERROR_INVALID_PARAMETER);
            const auto executable = resolver.ResolvePmRelativePath(app.executable).resolved_path;
            learning::LearningRequest request{app.directory, executable, app.arguments, app.id, {}};
            int i = 4;
            for (; i < argc && std::wstring_view(argv[i]) != L"--"; ++i) {
                if (i - 4 >= static_cast<int>(request.exclusive_paths.size())) return Status(ERROR_INVALID_PARAMETER);
                request.exclusive_paths[static_cast<std::size_t>(i - 4)] = argv[i];
            }
            if (i + 1 < argc) request.arguments += L" " + Arguments(i + 1, argc, argv);
            Handle lease;
            const DWORD acquired = AcquireApp(resolver, app.directory, lease);
            if (acquired) return Status(acquired);
            const auto result = learning::LearningEngine::Run(resolver, request);
            if (!result.wrapper_path.empty()) std::wprintf(L"%ls\n", result.wrapper_path.c_str());
            return Status(result.rollback_error ? result.rollback_error : result.error);
        }
        int argument_start = 3;
        if (argc > 3 && std::wstring_view(argv[3]) == L"--allow-red") ++argument_start;
        if (Equal(app.color, L"Red") && argument_start == 3) return Status(ERROR_ACCESS_DENIED);
        const Handle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) return Status(::GetLastError());
        StopRequest stop;
        stop.event = event.get();
        active_stop.store(&stop);
        ::SetConsoleCtrlHandler(StopConsole, TRUE);
        const auto result = Run(resolver, app, Arguments(argument_start, argc, argv), stop);
        ::SetConsoleCtrlHandler(StopConsole, FALSE);
        active_stop.store(nullptr);
        if (result.rollback_error || result.error)
            return Status(result.rollback_error ? result.rollback_error : result.error);
        return static_cast<int>(result.exit_code);
    }
    std::fwprintf(stderr,
        L"pml.exe --desktop | --list | --run <id> [argumentos]\n"
        L"        --register <id> <App/carpeta> <App/carpeta/app.exe> [nombre]\n"
        L"        --install <id> <paquete absoluto> [ejecutable relativo]\n"
        L"        --learn <id> --approve <recurso exclusivo> [recursos] [-- argumentos]\n"
        L"        --reset | --archive | --uninstall <id> --confirm\n");
    return command == L"--help" ? 0 : 2;
}

} // namespace pm::applications
