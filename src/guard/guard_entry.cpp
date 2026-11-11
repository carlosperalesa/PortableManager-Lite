#include "guard/guard_entry.h"

#include <windows.h>
#include <cstdio>

namespace pm::guard {

int RunGuardMode(std::wstring_view session_id, std::wstring_view pm_handle_arg) noexcept {
    if (session_id.empty()) {
        std::fwprintf(stderr, L"[PMGuard] Error: SessionID vacio en modo --guard.\n");
        return 2;
    }

    // Fase 0: Verificación de arranque e invocación del modo --guard.
    // La lógica completa de espera bloqueante (WaitForSingleObject), Mutex y limpieza crítica
    // se integra en las Fases 2 y 3.
    std::wprintf(L"[PMGuard] Fase 0 OK | SessionID=%.*s | HandleArg=%.*s\n",
                 static_cast<int>(session_id.size()), session_id.data(),
                 static_cast<int>(pm_handle_arg.size()),
                 pm_handle_arg.empty() ? L"NONE" : pm_handle_arg.data());
    return 0;
}

} // namespace pm::guard
