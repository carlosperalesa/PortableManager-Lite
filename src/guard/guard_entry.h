#pragma once

#include <string_view>

namespace pm::guard {

// Punto de entrada para el modo sombra de emergencia:
// pm.exe --guard <SessionID> [PmProcessHandleHex]
int RunGuardMode(std::wstring_view session_id, std::wstring_view pm_handle_arg) noexcept;

} // namespace pm::guard
