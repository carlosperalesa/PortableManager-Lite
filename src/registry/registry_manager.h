#pragma once

#include "runtime/session_manager.h"

namespace pm::registry {
class RegistryManager {
public:
    // The caller attributes the complete subtree to one application and starts
    // PMGuard first. Prepare must succeed before creating its Job or launching it.
    static bool Prepare(runtime::SessionManager& session, std::wstring_view exclusive_key,
                        DWORD& error, bool require_existing_parent = false);

    // Called after the Job drains. Only exports a subtree absent at baseline.
    // The export stays in the local session until PM publishes it to portable storage.
    static bool CaptureNewData(const runtime::SessionManager& session,
                               const runtime::ManifestOperation& op,
                               std::wstring& snapshot, DWORD& error);

    static bool IsRegistryOperation(const runtime::ManifestOperation& op,
                                    std::wstring_view session_dir);
    static bool Rollback(const runtime::ManifestOperation& op, DWORD& error);
    static bool ReleaseBackup(const runtime::ManifestOperation& op, DWORD& error);
};
}
