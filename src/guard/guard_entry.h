#pragma once
#include <string_view>
namespace pm::guard {
// Private shadow protocol: decimal inherited PM handle + readiness event handle.
int RunGuardMode(std::wstring_view session_id, std::wstring_view pm_handle_arg,
                 std::wstring_view ready_handle_arg = {});
}
