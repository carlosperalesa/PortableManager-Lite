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
| **Fase 0 / 0B** | `test_phase0_smoke.exe` | [reports/fase0b_native.json](reports/fase0b_native.json) | Compilación limpia C++20 x64 (`-static`, `-Wl,-Xlink=-swaprun:cd`), inspección PE con `llvm-readobj.exe` (`0x8664`, `0x400`, 0 DLLs externas del toolchain), arranque y salida normal, ejecución desde otro CWD (`%TEMP%`) verificando `GetModuleFileNameW`, e invocación `--guard`. | **`passed`** |
| **Fase 1** (`fase01/`) | `test_phase1_core.exe` | [reports/rebuild_core.json](reports/rebuild_core.json) | `PathResolver`, `AtomicFile`, `ConfigManager` e integración con copias de `pm.exe` en ubicaciones distintas. | **`passed`** |
| **Fase 2** (`fase02/`) | `test_phase2_runtime.exe` | [reports/rebuild_runtime.json](reports/rebuild_runtime.json), [reports/rebuild_runtime_audit.json](reports/rebuild_runtime_audit.json) | **23 escenarios** (16 base + 7 de auditoría final): creación de sesión, GUID único, manifiesto en `%TEMP%` sin `PmRootPath`, protocolo write-ahead, estados unificados (`INTENT_RECORDED`, `APPLIED`, `ROLLED_BACK`, `FAILED`, `SKIPPED`), caídas reales (`TerminateProcess`), orden efectivo de `KILL_JOB` primero en LIFO, representación y validación de respaldos `.reg` sobre clave sintética `HKCU`, preservación de respaldos `.pmbkp` y `.reg` ante fallos, apertura de sesión activa por PMG, ausencia de recuperación automática al iniciar PM y prohibición de eliminar sesiones con operaciones pendientes. | **`passed`** |
| **Fase 3** (`fase03/`) | `test_phase3_guard.exe` | [reports/fase03_windows_final_20261111.json](reports/fase03_windows_final_20261111.json), [reports/fase03_usb_audit_v2_20261111.json](reports/fase03_usb_audit_v2_20261111.json), [reports/fase03_usb_physical_disconnect_v2_20261112.json](reports/fase03_usb_physical_disconnect_v2_20261112.json) | `JobManager` y `PMGuard`: **11 escenarios** — PM activo (PMG no limpia), cierre normal (1 sola limpieza), caída abrupta de PM antes y durante el cierre crítico (`WAIT_ABANDONED`), pérdida de carpeta origen seguida de caída de PM, detección de pérdida del origen mientras PM permanece vivo (`WaitForReleaseOrOriginLoss` -> cierre crítico automático), consumo de CPU despreciable (`WaitForSingleObject` / `MsgWaitForMultipleObjects` sin polling de alta frecuencia), caída simultánea de hijos, muerte por cierre del último handle, barrera ante fallo de terminación, rechazos de seguridad y **desconexión física manual del USB `E:`**. | `passed` (11/11 grupos en disco interno, 11/11 en USB `E:` y desconexión física real `passed`; cierre aprobado en PR #2) |
| **Fase 4** (fase04/) | test_phase4_junctions.exe | [Informe JSON](reports/rebuild_junctions_windows_final.json) | SHA-256 del original, ausencia previa, destino inaccesible y huérfano, colisión de respaldo, sustituciones ajenas, interrupción entre renombrados, caída de PM y caída de PMGuard. | [Resultados](fase04/README_FASE04.md) |
| **Fase 5** (`fase05/`) | `test_phase5_registry.exe` | `reports/rebuild_registry.json` | `RegistryManager`: clave inexistente, clave existente (`REG_SZ`, `REG_DWORD`, `REG_BINARY`, `REG_MULTI_SZ`, subclaves), caída de PM y fallo de restauración conservando el respaldo. | `not_run` |
| **Fase 6** (`fase06/`) | `test_phase6_learning.exe` | `reports/rebuild_learning.json` | `LearningEngine`: metadatos PE (`GetFileVersionInfoW`), candidatos acotados, respaldo, atribución, preservación de cambios ambiguos y receta INI con rutas relativas. | `not_run` |
| **Fase 7** (`fase07/`) | `test_phase7_cleanup.exe` | `reports/rebuild_cleanup.json` | `CleanupEngine`: modos `NORMAL`, `FAST` (200 ms) y `CRITICAL`, y creación de `%TEMP%\pmlog` sin extensión solo ante residuos no restaurables. | `not_run` |
| **Fase 8** (`fase08/`) | `test_phase8_integration.exe` | `reports/rebuild_integration.json` | Ciclo completo extremo a extremo con aplicación sintética controlada desde consola. | `not_run` |

## Evidencia histórica de la implementación de Fase 3 (12 de noviembre de 2026)

La suite `test_phase3_guard.exe` se compila con CMake y `build.ps1`; sus nueve grupos
se describen en `fase03/IMPLEMENTACION_FASE_3.md`. El script Windows genera un JSON
nuevo por ejecución con estados obtenidos de las pruebas. `reports/rebuild_guard_cloud_20261112.json`
registra únicamente lo comprobado en Linux y los bloqueos; no es evidencia Win32.
Se requieren regresiones 0B, 1 y 2 por el cambio del protocolo `--guard` y el manejo
de errores del rollback. La prueba de pérdida del origen es sintética, no USB físico.

Actualización: Actions 43130358371 ejecutó las suites reales en Windows: Fase 3
9/9 grupos y regresiones 0B/1/2 `passed` (23 escenarios de Fase 2). Se conserva la
evidencia cloud anterior como histórica. Resultados y diagnósticos en
`reports/fase03_actions_43130358371.json` y `fase03/INFORME_FASE03.md`.
