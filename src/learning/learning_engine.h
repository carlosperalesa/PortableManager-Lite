#pragma once

#include "core/path_resolver.h"

#include <windows.h>
#include <array>
#include <optional>

namespace pm::learning {

struct Candidates {
    std::wstring company;
    std::wstring product;
    std::wstring roaming;
    std::wstring local;
    std::wstring registry;
};

struct LearningRequest {
    // Existing installation directory relative to the PML root. The absolute
    // executable path must point to a regular file inside this directory.
    std::wstring app_dir;
    std::wstring executable;
    std::wstring arguments;
    std::wstring app_id;
    // Supply only exact candidate strings backed by independent evidence of
    // exclusive use, including the absence of external writers during this run.
    // PE metadata and an absent baseline are insufficient evidence.
    std::array<std::wstring, 3> exclusive_paths;
    DWORD timeout_ms = INFINITE;
};

struct LearningResult {
    bool host_restored = false;
    DWORD error = ERROR_SUCCESS;
    DWORD rollback_error = ERROR_SUCCESS;
    std::wstring session_id;
    std::wstring data_path;
    std::wstring wrapper_path;
};

class LearningEngine {
public:
    static std::optional<Candidates> Inspect(const std::wstring& executable);

    // Phase 6 API for a dedicated learning process. A reservation persists in
    // its PMGuard until the process exits. Unapproved candidates are untouched
    // by PM, including when other candidates are approved for this execution.
    static LearningResult Run(const core::PathResolver& resolver, const LearningRequest& request);
};

}
