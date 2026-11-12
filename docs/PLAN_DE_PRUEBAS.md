# Plan de Pruebas de Reconstrucción Nativa (C++20 / LLVM-MinGW) — PortableManager Lite

**Fecha:** 11 de noviembre de 2026

**Toolchain Oficial:** `tools/toolchains/llvm-mingw/bin/clang++.exe` (LLVM-MinGW 20261104, Clang 23.1.5, UCRT, Windows x86_64).

---

## 1. Reglas Generales de Prueba (Sección 14)

- Todas las pruebas se compilan en **C++20 nativo** (`-std=c++20 -O2 -Wall -Wextra -Werror -municode -static`) y se ejecutan sobre directorios sintéticos en `%TEMP%` y claves exclusivas `HKCU\Software\PortableManager\Tests\<ID>`.
- Para probar interrupciones abruptas, se lanzan procesos auxiliares reales mediante `CreateProcessW` que se terminan deliberadamente con `TerminateProcess(::GetCurrentProcess(), ...)` en el punto exacto de fallo.
- Prohibido tocar carpetas o claves reales de Firefox, PuTTY u otras aplicaciones instaladas en el anfitrión.
- Prohibido conectar o desconectar físicamente el USB sin autorización expresa del usuario.
- Cada fase genera su propio informe JSON y carpeta consolidada de dos dígitos (`docs/fase01/` .. `docs/fase08/`) sin sobrescribir los informes históricos previos.

---

## 2. Matriz de Pruebas por Fase

| Fase | Ejecutable de Prueba (C++20) | Informe JSON Objetivo | Escenarios Verificados | Estado Actual |
|---|---|---|---|---|
| **Fase 0 / 0B** | `test_phase0_smoke.exe` | [reports/fase0b_native.json](../reports/fase0b_native.json) | Compilación limpia C++20 x64 (`-static`, `-Wl,-Xlink=-swaprun:cd`), inspección PE con `llvm-readobj.exe` (`0x8664`, `0x400`, 0 DLLs externas del toolchain), arranque y salida normal, ejecución desde otro CWD (`%TEMP%`) verificando `GetModuleFileNameW`, e invocación `--guard`. | **`passed`** |
| **Fase 1** (`docs/fase01/`) | `test_phase1_core.exe` | [reports/rebuild_core.json](../reports/rebuild_core.json) | `PathResolver`, `AtomicFile`, `ConfigManager` e integración con copias de `pml.exe` en ubicaciones distintas. | **`passed`** |
| **Fase 2** (`docs/fase02/`) | `test_phase2_runtime.exe` | [reports/rebuild_runtime.json](../reports/rebuild_runtime.json), [reports/rebuild_runtime_audit.json](../reports/rebuild_runtime_audit.json) | **23 escenarios** (16 base + 7 de auditoría final): creación de sesión, GUID único, manifiesto en `%TEMP%` sin `PmRootPath`, protocolo write-ahead, estados unificados (`INTENT_RECORDED`, `APPLIED`, `ROLLED_BACK`, `FAILED`, `SKIPPED`), caídas reales (`TerminateProcess`), orden efectivo de `KILL_JOB` primero en LIFO, representación y validación de respaldos `.reg` sobre clave sintética `HKCU`, preservación de respaldos `.pmbkp` y `.reg` ante fallos, apertura de sesión activa por PMG, ausencia de recuperación automática al iniciar PM y prohibición de eliminar sesiones con operaciones pendientes. | **`passed`** |
| **Fase 3** (`docs/fase03/`) | `test_phase3_guard.exe` | [reports/fase03_windows_final_20261111.json](../reports/fase03_windows_final_20261111.json), [reports/fase03_usb_audit_v2_20261111.json](../reports/fase03_usb_audit_v2_20261111.json), [reports/fase03_usb_physical_disconnect_v2_20261112.json](../reports/fase03_usb_physical_disconnect_v2_20261112.json) | `JobManager` y `PMGuard`: **11 escenarios** — PM activo (PMG no limpia), cierre normal (1 sola limpieza), caída abrupta de PM antes y durante el cierre crítico (`WAIT_ABANDONED`), pérdida de carpeta origen seguida de caída de PM, detección de pérdida del origen mientras PM permanece vivo (`WaitForReleaseOrOriginLoss` -> cierre crítico automático), consumo de CPU despreciable (`WaitForSingleObject` / `MsgWaitForMultipleObjects` sin polling de alta frecuencia), caída simultánea de hijos, muerte por cierre del último handle, barrera ante fallo de terminación, rechazos de seguridad y **desconexión física manual del USB `E:`**. | `passed` (11/11 grupos en disco interno, 11/11 en USB `E:` y desconexión física real `passed`; cierre aprobado en PR #2) |
| **Fase 4** (`docs/fase04/`) | test_phase4_junctions.exe | [Informe JSON](../reports/rebuild_junctions_windows_final.json) | SHA-256 del original, ausencia previa, destino inaccesible y huérfano, colisión de respaldo, sustituciones ajenas, interrupción entre renombrados, caída de PM y caída de PMGuard. | [Resultados](fase04/README_FASE04.md) |
| **Fase 5** (`docs/fase05/`) | `test_phase5_registry.exe` | `reports/rebuild_registry.json` | `RegistryManager`: clave inexistente, clave existente (`REG_SZ`, `REG_DWORD`, `REG_BINARY`, `REG_MULTI_SZ`, subclaves), caída de PM y fallo de restauración conservando el respaldo. | `not_run` |
| **Fase 6** (`docs/fase06/`) | `test_phase6_learning.exe` | [Evidencia Windows](../reports/rebuild_learning.json) | Metadatos PE, candidatos acotados, detección de modificaciones reales, wrapper operativo sin cosecha de datos, restauración, exclusión de cambios ambiguos, cierre crítico sin escrituras al USB y recuperación por PMGuard. | `passed` (6/6 con alcance redefinido, CI [43245395915](https://github.com/carlosperalesa/PortableManager-Lite/actions/runs/43245395915); Fases 0B–5 también pasan) |
| **Fase 7** (`docs/fase07/`) | `test_phase7_cleanup.exe` | [Evidencia](../reports/rebuild_cleanup.json) | Modos `NORMAL`, `FAST` (200 ms) y `CRITICAL`, barrera de Jobs, protección de recursos ajenos, respaldos, reintentos, `pmlog` sin extensión y recuperación por PMGuard ante pérdida de origen simulada. | 10/10 en Windows y regresiones 0B–6 superadas. Desconexión física USB `not_run`. |
| **Fase 8** (`docs/fase08/`) | `test_phase8_integration.exe` | `reports/ci_windows_native.json` (artefacto CI) | Aplicación sintética bajo Job, aprendizaje, actualización con colisión de wrapper, recuperación por PMG, PMD, mantenimiento, un guardián con cero o varias apps, cierre unitario, reposición y agotamiento de reintentos. | [Evidencia](fase08/README_FASE08.md) |

## Evidencia histórica de la implementación de Fase 3 (12 de noviembre de 2026)

La suite `test_phase3_guard.exe` se compila con CMake y `build.ps1`; sus nueve grupos
se describen en [implementación de Fase 3](fase03/IMPLEMENTACION_FASE_3.md). El script Windows genera un JSON
nuevo por ejecución con estados obtenidos de las pruebas. `reports/rebuild_guard_cloud_20261112.json`
registra únicamente lo comprobado en Linux y los bloqueos; no es evidencia Win32.
Se requieren regresiones 0B, 1 y 2 por el cambio del protocolo `--guard` y el manejo
de errores del rollback. La prueba de pérdida del origen es sintética, no USB físico.

Actualización: Actions 43130358371 ejecutó las suites reales en Windows: Fase 3
9/9 grupos y regresiones 0B/1/2 `passed` (23 escenarios de Fase 2). Se conserva la
evidencia cloud anterior como histórica. Resultados y diagnósticos en
`reports/fase03_actions_43130358371.json` y [informe de Fase 3](fase03/INFORME_FASE03.md).

## Regresión previa a Fase 8

Validar `pml.exe` y la copia temporal del mismo binario en modo `--guard`.
La suite de Fase 6 prueba `wrapper.ini` en el directorio de instalación,
preservación de un wrapper previo, aislamiento entre instalaciones, rechazo de
traversal y puntos de reanálisis, y rollback incluso si falla la publicación.
La suite de Fase 7 verifica el retiro del ejecutable temporal con PML activo
y el residuo documentado tras recuperación abrupta. Ejecutar 0B–7 en Windows
y compilación cruzada e inspección del PE en GitHub Actions. La desconexión
física USB para esta refactorización queda `not_run` por decisión del propietario.
