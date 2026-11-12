#pragma once
#include "runtime/session_manager.h"

namespace pm::guard { class GuardProcess; }

namespace pm::cleanup {

enum class Mode { Normal, Fast, Critical };
enum class Artifacts { Remove, KeepManifest };

inline constexpr DWORD kFastGraceMs = 200;
inline constexpr DWORD kNormalGraceMs = 5000;

struct Result {
    bool ok = false;
    DWORD error = ERROR_SUCCESS;
    DWORD log_error = ERROR_SUCCESS;
};

class CleanupEngine {
public:
    static Result Run(runtime::SessionManager& session, Mode mode,
                      Artifacts artifacts = Artifacts::Remove,
                      guard::GuardProcess* guard = nullptr);
    static Result RunApplication(runtime::SessionManager& session, std::wstring_view app_id, Mode mode);
    // PMGuard can call this only after its monitored owner exits.
    static Result Recover(runtime::SessionManager& session, bool managed_shadow);
};

} // namespace pm::cleanup
