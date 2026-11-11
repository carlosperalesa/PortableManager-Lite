# IMPLEMENTACIÓN FASE 1: CORE Y RUTAS (C++20 NATIVO)

**Fecha:** 2026-11-11  
**Estado:** `passed` (12/12 subpruebas superadas en `tests/test_phase1_core.cpp`)  
**Toolchain:** LLVM-MinGW `20261104` (Clang `23.1.5`, UCRT, Windows `x86_64`)

---

## 1. Módulos Implementados en `src/core/`

1. **`PathResolver`** ([`src/core/path_resolver.h`](src/core/path_resolver.h), [`src/core/path_resolver.cpp`](src/core/path_resolver.cpp)):
   - Determinación exclusiva de la raíz de PM mediante `GetModuleFileNameW(nullptr)` (nunca usa el `CWD`).
   - Normalización de separadores y resolución segura de rutas relativas a `pm.exe`.
   - Rechazo estricto de escapes fuera de la raíz (`..`), rutas absolutas, letras de unidad y rutas UNC en configuraciones persistentes.
   - Resolución separada de tokens del anfitrión (`%APPDATA%`, `%LOCALAPPDATA%`, `%TEMP%`, `%USERPROFILE%`) para uso exclusivo en recetas operativas.

2. **`AtomicFile`** ([`src/core/atomic_file.h`](src/core/atomic_file.h), [`src/core/atomic_file.cpp`](src/core/atomic_file.cpp)):
   - Escritura atómica en el mismo directorio (`<destino>.pmtmp.<PID>.<Tick>`) mediante `CreateFileW` + `WriteFile` + `FlushFileBuffers` + `CloseHandle` + `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.
   - Preservación garantizada del archivo destino previo si falla cualquier etapa de escritura.

3. **`ConfigManager`** ([`src/core/config_manager.h`](src/core/config_manager.h), [`src/core/config_manager.cpp`](src/core/config_manager.cpp)):
   - Parser/serializador INI determinista en UTF-8 que preserva el orden de secciones, claves y comentarios.
   - Gestión de `Data/config.ini` y `Data/apps.ini` con bloqueo estricto de rutas absolutas del anfitrión.
   - Escritura únicamente cuando existen cambios efectivos (`dirty`).

---

## 2. Evidencia de Pruebas

- Suite de pruebas: [`tests/test_phase1_core.cpp`](tests/test_phase1_core.cpp).
- Informe JSON: [`fase01/rebuild_core.json`](fase01/rebuild_core.json) y [`reports/rebuild_core.json`](reports/rebuild_core.json).
