#pragma once

#include "runtime/session_manager.h"

namespace pm::registry {
class RegistryManager {
public:
    // The caller attributes the complete subtree to one application and starts
    // PMGuard first. Prepare must succeed before creating its Job or launching it.
    static bool Prepare(runtime::SessionManager& session, std::wstring_view exclusive_key,
                        DWORD& error);

    static bool IsRegistryOperation(const runtime::ManifestOperation& op,
                                    std::wstring_view session_dir);
    static bool Rollback(const runtime::ManifestOperation& op, DWORD& error);
    static bool ReleaseBackup(const runtime::ManifestOperation& op, DWORD& error);
};
}
