# Fase 01 — Core y Rutas (`PathResolver`, `AtomicFile` y `ConfigManager`)

**Estado:** `passed`  
**Toolchain:** LLVM-MinGW `20261104` (Clang `23.1.5`, UCRT, Windows `x86_64`)

---

## Contenido de la Carpeta `fase01/`

- [`README_FASE01.md`](file:///d:/Data/fase01/README_FASE01.md) — Índice de la Fase 1.
- [`rebuild_core.json`](file:///d:/Data/fase01/rebuild_core.json) — Evidencia JSON de validación de Fase 1 ([`reports/rebuild_core.json`](file:///d:/Data/reports/rebuild_core.json)).
- Módulos implementados en [`src/core/`](file:///d:/Data/src/core):
  - [`path_resolver.h`](file:///d:/Data/src/core/path_resolver.h) y [`path_resolver.cpp`](file:///d:/Data/src/core/path_resolver.cpp)
  - [`atomic_file.h`](file:///d:/Data/src/core/atomic_file.h) y [`atomic_file.cpp`](file:///d:/Data/src/core/atomic_file.cpp)
  - [`config_manager.h`](file:///d:/Data/src/core/config_manager.h) y [`config_manager.cpp`](file:///d:/Data/src/core/config_manager.cpp)
- Suite de pruebas: [`tests/test_phase1_core.cpp`](file:///d:/Data/tests/test_phase1_core.cpp).
