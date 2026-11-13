#pragma once

#include "runtime/session_manager.h"

namespace pm::registry {
class RegistryManager {
public:
    // The caller attributes the complete subtree to one application and starts
    // PMGuard first. Prepare must succeed before creating its Job or launching it.
    static bool Prepare(runtime::SessionManager& session, std::wstring_view exclusive_key,
                        DWORD& error, bool require_existing_parent = false, std::wstring_view app_id = {});

    // Observe whether an attributed subtree differs from its pre-launch
    // baseline. Any temporary comparison export stays in %TEMP% and is
    // discarded; never copies application settings to portable storage.
    static bool HasChanges(const runtime::SessionManager& session,
                           const runtime::ManifestOperation& op,
                           bool& changed, DWORD& error);

    static bool IsRegistryOperation(const runtime::ManifestOperation& op,
                                    std::wstring_view session_dir);
    static bool Rollback(const runtime::ManifestOperation& op, DWORD& error);
    static bool ReleaseBackup(const runtime::ManifestOperation& op, DWORD& error);

    // Optional portable registry persistence (when PersistRegKey=true in wrapper.ini).
    // Validates key scope and .reg section scope before exporting/importing.
    static bool ExportPortableData(std::wstring_view key, std::wstring_view reg_file, DWORD& error);
    static bool ImportPortableData(std::wstring_view key, std::wstring_view reg_file, DWORD& error);
};
}
