#pragma once

#include "runtime/session_manager.h"

namespace pm::core { class PathResolver; }

namespace pm::junctions {

class JunctionManager {
public:
    // The caller has identified an exclusive host directory and started PMGuard.
    // Prepare before registering the app's Job. On failure, run CleanupSession.
    // data_relative_path must be an existing directory under Data/apps/<app_id>.
    static bool Prepare(runtime::SessionManager& session, const core::PathResolver& resolver,
                        const std::wstring& app_id, const std::wstring& exclusive_host_path,
                        const std::wstring& data_relative_path, DWORD& error);

    // Called under the session mutex after all controlled processes have stopped.
    static bool Rollback(const runtime::ManifestOperation& op, DWORD& error);

    static bool IsJunctionOperation(const runtime::ManifestOperation& op);
};

} // namespace pm::junctions
