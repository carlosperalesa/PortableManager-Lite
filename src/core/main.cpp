#include "core/atomic_file.h"
#include "core/config_manager.h"
#include "core/path_resolver.h"
#include "core/pm_version.h"
#include "guard/guard_entry.h"
#include "guard/guard_process.h"
#include "jobs/job_manager.h"
#include "runtime/runtime_module.h"

#include <cstdio>
#include <cwchar>
#include <string>
#include <string_view>
#include <windows.h>


int wmain(int argc, wchar_t *argv[]) {
  const pm::core::PathResolver resolver;
  const std::wstring &exe_dir = resolver.root_dir();
  if (exe_dir.empty()) {
    std::fwprintf(stderr,
                  L"[PM] Error critico: GetModuleFileNameW fallo (err=%lu).\n",
                  ::GetLastError());
    return 1;
  }

  if (argc >= 2) {
    const std::wstring_view arg1(argv[1]);

    if (arg1 == L"--guard") {
      const std::wstring_view session_id =
          (argc >= 3) ? std::wstring_view(argv[2]) : std::wstring_view{};
      const std::wstring_view handle_arg =
          (argc >= 4) ? std::wstring_view(argv[3]) : std::wstring_view{};
      const std::wstring_view ready_arg =
          (argc >= 5 && argc <= 6) ? std::wstring_view(argv[4]) : std::wstring_view{};
      const std::wstring_view stop_arg = argc == 6 ? std::wstring_view(argv[5]) : std::wstring_view{};
      return pm::guard::RunGuardMode(session_id, handle_arg, ready_arg, stop_arg);
    }

    if (arg1 == L"--version") {
      std::wprintf(L"%ls v%ls (x64 Native Win32)\n", pm::core::kProductName,
                   pm::core::kVersionString);
      return 0;
    }

    if (arg1 == L"--smoke-test") {
      const auto cfg_path = resolver.ResolvePmRelativePath(
          pm::core::ConfigManager::kDefaultConfigRelPath);
      if (!cfg_path.ok) {
        return 2;
      }
      std::wprintf(L"[PM] SMOKE_OK | Version=%ls | Root=%ls | ConfigPath=%ls\n",
                   pm::core::kVersionString, exe_dir.c_str(),
                   cfg_path.resolved_path.c_str());
      return 0;
    }

    if (arg1 == L"--init-config") {
      pm::core::ConfigManager cfg_mgr(resolver);
      auto load_cfg = cfg_mgr.LoadConfigIni(true);
      if (!load_cfg.ok) {
        std::fwprintf(stderr, L"[PM] Error cargando config.ini: %ls\n",
                      load_cfg.error_message.c_str());
        return 3;
      }
      std::wstring err;
      if (!cfg_mgr.config_doc().HasKey(L"General", L"Version")) {
        cfg_mgr.config_doc().SetValue(L"General", L"Version",
                                      pm::core::kVersionString, err);
      }
      if (!cfg_mgr.config_doc().HasKey(L"General", L"AppsDir")) {
        cfg_mgr.config_doc().SetRelativePathValue(L"General", L"AppsDir",
                                                  L"App", err);
      }
      if (!cfg_mgr.config_doc().HasKey(L"General", L"DataDir")) {
        cfg_mgr.config_doc().SetRelativePathValue(L"General", L"DataDir",
                                                  L"Data", err);
      }

      auto save_cfg = cfg_mgr.SaveConfigIniIfModified(true);
      if (!save_cfg.ok) {
        std::fwprintf(stderr, L"[PM] Error guardando config.ini: %ls\n",
                      save_cfg.error_message.c_str());
        return 4;
      }

      auto load_apps = cfg_mgr.LoadAppsIni(true);
      if (!load_apps.ok) {
        std::fwprintf(stderr, L"[PM] Error cargando apps.ini: %ls\n",
                      load_apps.error_message.c_str());
        return 5;
      }
      auto save_apps = cfg_mgr.SaveAppsIniIfModified(true);
      if (!save_apps.ok) {
        std::fwprintf(stderr, L"[PM] Error guardando apps.ini: %ls\n",
                      save_apps.error_message.c_str());
        return 6;
      }

      std::wprintf(
          L"[PM] CONFIG_OK | Root=%ls | WroteConfig=%d | WroteApps=%d\n",
          exe_dir.c_str(), save_cfg.wrote_to_disk ? 1 : 0,
          save_apps.wrote_to_disk ? 1 : 0);
      return 0;
    }

    // Phase-3 diagnostic, not application management (phase 8). The controlled
    // process is our own smoke probe; no real user application is touched.
    if (arg1 == L"--job-probe") {
      const std::wstring exe_path =
          pm::core::PathResolver::QueryExecutablePath();
      if (!pm::guard::IsOriginAccessible(exe_path))
        return 10;
      pm::runtime::SessionManager session;
      if (!session.InitializeNewSession().ok)
        return 10;
      pm::jobs::JobManager job;
      pm::guard::GuardProcess guard;
      DWORD error = ERROR_SUCCESS;
      const auto close = [&](pm::cleanup::Mode mode) {
        return pm::cleanup::CleanupEngine::Run(
            session, mode, pm::cleanup::Artifacts::Remove, &guard);
      };
      if (!job.Create(session, L"probe", error)) {
        close(pm::cleanup::Mode::Critical);
        return 11;
      }
      const std::wstring temp = pm::runtime::SessionManager::QuerySystemTempDir();
      if (!::SetCurrentDirectoryW(temp.c_str()) ||
          !guard.Start(exe_path, session, error)) {
        close(pm::cleanup::Mode::Critical);
        return 12;
      }
      PROCESS_INFORMATION process{};
      if (!job.Launch(exe_path, L"--smoke-test", temp, process, error)) {
        close(pm::cleanup::Mode::Critical);
        return 13;
      }
      const auto waited = pm::guard::WaitForReleaseOrOriginLoss(
          exe_path, process.hProcess, 10000, error);
      DWORD code = 1;
      ::GetExitCodeProcess(process.hProcess, &code);
      ::CloseHandle(process.hThread);
      ::CloseHandle(process.hProcess);
      const auto mode = waited == pm::guard::OriginWaitResult::Released
          ? pm::cleanup::Mode::Normal : pm::cleanup::Mode::Critical;
      if (!close(mode).ok || waited != pm::guard::OriginWaitResult::Released || code != 0)
        return 14;
      std::wprintf(L"[PM] JOB_PROBE_OK | SessionId=%ls\n", session.session_id().c_str());
      return 0;
    }

    if (arg1 == L"--session-probe") {
      pm::runtime::SessionManager session_mgr;
      auto init_res = session_mgr.InitializeNewSession();
      if (!init_res.ok) {
        std::fwprintf(stderr, L"[PM] Error inicializando sesion: %ls\n",
                      init_res.error_message.c_str());
        return 7;
      }
      if (!session_mgr.TransitionState(pm::runtime::SessionState::Active).ok ||
          !session_mgr.TransitionState(pm::runtime::SessionState::Cleaning)
               .ok ||
          !session_mgr.TransitionState(pm::runtime::SessionState::Cleaned).ok) {
        return 8;
      }
      DWORD clean_err = ERROR_SUCCESS;
      if (!session_mgr.FinalizeSessionAndCleanupOwnFiles(true, clean_err)) {
        return 9;
      }
      std::wprintf(L"[PM] SESSION_PROBE_OK | SessionId=%ls\n",
                   init_res.session_id.c_str());
      return 0;
    }
  }

  std::wprintf(L"%ls v%ls | Root=%ls | Ready.\n", pm::core::kProductName,
               pm::core::kVersionString, exe_dir.c_str());
  return 0;
}
