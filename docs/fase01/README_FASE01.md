# Fase 01 — Core y Rutas (`PathResolver`, `AtomicFile` y `ConfigManager`)

**Estado:** `passed`

**Toolchain:** LLVM-MinGW `20261104` (Clang `23.1.5`, UCRT, Windows `x86_64`)

---

## Contenido de la Carpeta `docs/fase01/`

- [`IMPLEMENTACION_FASE_1.md`](IMPLEMENTACION_FASE_1.md) — Implementación de la Fase 1.
- [`rebuild_core.json`](rebuild_core.json) — Evidencia JSON de validación de Fase 1 ([`reports/rebuild_core.json`](../../reports/rebuild_core.json)).
- Módulos implementados en [`src/core/`](../../src/core):
  - [`path_resolver.h`](../../src/core/path_resolver.h) y [`path_resolver.cpp`](../../src/core/path_resolver.cpp)
  - [`atomic_file.h`](../../src/core/atomic_file.h) y [`atomic_file.cpp`](../../src/core/atomic_file.cpp)
  - [`config_manager.h`](../../src/core/config_manager.h) y [`config_manager.cpp`](../../src/core/config_manager.cpp)
- Suite de pruebas: [`tests/test_phase1_core.cpp`](../../tests/test_phase1_core.cpp).
