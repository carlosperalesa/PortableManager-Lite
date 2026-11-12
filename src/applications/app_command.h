#pragma once

#include "core/path_resolver.h"

#include <string_view>

namespace pm::applications {

int RunCommand(const core::PathResolver& resolver, std::wstring_view command, int argc, wchar_t** argv);

} // namespace pm::applications
