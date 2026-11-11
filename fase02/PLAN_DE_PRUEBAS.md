# Plan de Pruebas de Reconstrucción Nativa (C++20 / LLVM-MinGW) — PortableManager Lite

**Fecha:** 11 de noviembre de 2026  
**Toolchain Oficial:** `tools/toolchains/llvm-mingw/bin/clang++.exe` (LLVM-MinGW 20261104, Clang 23.1.5, UCRT, Windows x86_64).

---

## 1. Reglas Generales de Prueba (Sección 14)

- Todas las pruebas se compilan en **C++20 nativo** (`-std=c++20 -O2 -Wall -Wextra -Werror -municode -static`) y se ejecutan sobre directorios sintéticos en `%TEMP%` y claves exclusivas `HKCU\Software\PortableManager\Tests\<ID>`.
- Para probar interrupciones abruptas, se lanzan procesos auxiliares reales mediante `CreateProcessW` que se terminan deliberadamente con `TerminateProcess(::GetCurrentProcess(), ...)` en el punto exacto de fallo.
- Prohibido tocar carpetas o claves reales de Firefox, PuTTY u otras aplicaciones instaladas en el anfitrión.
- Prohibido conectar o desconectar físicamente el USB sin autorización expresa del usuario.
- Cada fase genera su propio informe JSON y carpeta consolidada de dos dígitos (`fase01/` .. `fase08/`) sin sobrescribir los informes históricos previos.

---

## 2. Matriz de Pruebas por Fase

| Fase | Ejecutable de Prueba (C++20) | Informe JSON Objetivo | Escenarios Verificados | Estado Actual |
|---|---|---|---|---|
| **Fase 0 / 0B** | `test_phase0_smoke.exe` | [reports/fase0b_native.json](file:///d:/Data/reports/fase0b_native.json) | Compilación limpia C++20 x64 (`-static`, `-Wl,-Xlink=-swaprun:cd`), inspección PE con `llvm-readobj.exe` (`0x8664`, `0x400`, 0 DLLs externas del toolchain), arranque y salida normal, ejecución desde otro CWD (`%TEMP%`) verificando `GetModuleFileNameW`, e invocación `--guard`. | **`passed`** |
| **Fase 1** (`fase01/`) | `test_phase1_core.exe` | [reports/rebuild_core.json](file:///d:/Data/reports/rebuild_core.json) | `PathResolver`, `AtomicFile`, `ConfigManager` e integración con copias de `pm.exe` en ubicaciones distintas. | **`passed`** |
| **Fase 2** (`fase02/`) | `test_phase2_runtime.exe` | [reports/rebuild_runtime.json](file:///d:/Data/reports/rebuild_runtime.json), [reports/rebuild_runtime_audit.json](file:///d:/Data/reports/rebuild_runtime_audit.json) | **23 escenarios** (16 base + 7 de auditoría final): creación de sesión, GUID único, manifiesto en `%TEMP%` sin `PmRootPath`, protocolo write-ahead, estados unificados (`INTENT_RECORDED`, `APPLIED`, `ROLLED_BACK`, `FAILED`, `SKIPPED`), caídas reales (`TerminateProcess`), orden efectivo de `KILL_JOB` primero en LIFO, representación y validación de respaldos `.reg` sobre clave sintética `HKCU`, preservación de respaldos `.pmbkp` y `.reg` ante fallos, apertura de sesión activa por PMG, ausencia de recuperación automática al iniciar PM y prohibición de eliminar sesiones con operaciones pendientes. | **`passed`** |
| **Fase 3** (`fase03/`) | `test_phase3_guard.exe` | `reports/rebuild_guard.json` | `JobManager` y `PMGuard`: PM activo (PMG no limpia), cierre normal (1 sola limpieza), caída abrupta de PM, pérdida de carpeta origen, consumo de CPU despreciable (`WaitForSingleObject` sin polling), caída simultánea de hijos. | `not_run` |
| **Fase 4** (`fase04/`) | `test_phase4_junctions.exe` | `reports/rebuild_junctions.json` | `JunctionManager`: carpeta original existente (`.pmbkp` + SHA-256), carpeta inexistente, destino inaccesible, Junction huérfana, caída de PM y caída de PMG. | `not_run` |
| **Fase 5** (`fase05/`) | `test_phase5_registry.exe` | `reports/rebuild_registry.json` | `RegistryManager`: clave inexistente, clave existente (`REG_SZ`, `REG_DWORD`, `REG_BINARY`, `REG_MULTI_SZ`, subclaves), caída de PM y fallo de restauración conservando el respaldo. | `not_run` |
| **Fase 6** (`fase06/`) | `test_phase6_learning.exe` | `reports/rebuild_learning.json` | `LearningEngine`: metadatos PE (`GetFileVersionInfoW`), candidatos acotados, respaldo, atribución, preservación de cambios ambiguos y receta INI con rutas relativas. | `not_run` |
| **Fase 7** (`fase07/`) | `test_phase7_cleanup.exe` | `reports/rebuild_cleanup.json` | `CleanupEngine`: modos `NORMAL`, `FAST` (200 ms) y `CRITICAL`, y creación de `%TEMP%\pmlog` sin extensión solo ante residuos no restaurables. | `not_run` |
| **Fase 8** (`fase08/`) | `test_phase8_integration.exe` | `reports/rebuild_integration.json` | Ciclo completo extremo a extremo con aplicación sintética controlada desde consola. | `not_run` |
