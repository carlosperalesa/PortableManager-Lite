# Informe de implementación — Fase 03

Entrega del 12 de noviembre de 2026 (fecha del registro UTC). Rama nueva:
`feat/fase03-jobobjects-pmguard`, creada desde
`c3620e324f406eadfea5bb9c2178ab7dd67f259a` con árbol inicialmente limpio.

## Resultado conseguido

Implementados JobManager y PMGuard nativos en C++20 sobre las interfaces de
sesión/manifiesto existentes. El lanzamiento está suspendido hasta asignar el
proceso al Job; sus descendientes quedan controlados por Windows. El guard es
una copia del mismo PM en TEMP, recibe solo dos handles heredados mediante una
lista explícita y espera bloqueado al proceso PM. La limpieza comparte mutex y
estado persistido, valida la pertenencia de los Jobs y rechaza operaciones de
fases futuras. Se incluye `--job-probe` para diagnóstico sintético en Windows.

El rollback deja de continuar si falla KILL_JOB y comprueba los fallos de escritura
antes de informar éxito. No se han incorporado dependencias ni reinstalado herramientas.

**Estado de entrega:** implementación realizada y compilación cruzada verificada;
validación de ejecución Windows bloqueada en esta sesión Linux. Fase 3 no se
considera aprobada. Fases 0B, 1 y 2 conservan su aprobación histórica, sin afirmar
que se hayan vuelto a ejecutar sus suites.

## Cambios revisables

Archivos creados:

- `src/jobs/job_manager.h`, `src/jobs/job_manager.cpp`.
- `src/guard/guard_process.h`, `src/guard/guard_process.cpp`.
- `src/runtime/session_lock.h`.
- `tests/test_phase3_guard.cpp`.
- `fase03/IMPLEMENTACION_FASE_3.md`, este informe y el JSON consolidado de Fase 3.
- `reports/rebuild_guard_cloud_20261112.json`.

Archivos modificados:

- `src/core/main.cpp`, `src/guard/guard_entry.h`, `src/guard/guard_entry.cpp`,
  `src/jobs/jobs_module.h`, `src/runtime/manifest_manager.cpp`.
- `tests/test_phase0_smoke.cpp`: el protocolo real ya no admite el antiguo stub.
- `CMakeLists.txt`, `build.ps1`, `.github/workflows/portablemanager-ci.yml`.
- `build_msvc.ps1`: exige CMake en lugar del fallback incompleto de fuentes.
- `README.md`, `IMPLEMENTACION_PM_LITE.md`, `PLAN_DE_PRUEBAS.md`,
  `RESULTADOS_RECONSTRUCCION.md`, `docs/CODEX_CLOUD.md`, `fase03/README_FASE03.md`.

No se modificaron AGENTS.md, los informes históricos ni los consolidados de
fases anteriores. Los cambios quedan en el árbol de la rama nueva, sin commits,
push ni fusión. Se mantiene la restricción de no hacer commits de la sesión.

## Compilación y verificaciones realizadas

Comando de compilación ejecutado con el toolchain existente:

```bash
export PATH=/workspace/.pm-cloud-tools/bin:$PATH
bash tools/build-codex-cloud.sh
```

Resultado: código 0, `CROSS_COMPILE_PASSED`. Se compilaron `pm.exe`,
`test_phase0_smoke.exe`, `test_phase1_core.exe`, `test_phase2_runtime.exe` y
`test_phase3_guard.exe` con C++20, `-Wall -Wextra -Werror`, `-static`, `-municode`
y `/SWAPRUN:CD` en el flujo CMake. No hubo errores ni advertencias de compilación.

La inspección independiente con `llvm-readobj --file-headers` y `--coff-imports`
confirmó en los cinco artefactos AMD64, `IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP` y
ninguna DLL externa del toolchain. Los tamaños, imports y SHA-256 están en el JSON.

`ctest --test-dir build/codex-cloud -N` registra cuatro suites. Es una enumeración,
no una ejecución. `git diff --check` y `bash -n tools/build-codex-cloud.sh` pasaron.
No se ejecutaron PowerShell, CI remoto, EXE Windows ni pruebas físicas USB.

## Pruebas preparadas y regresión pendiente

La nueva suite contiene nueve grupos sintéticos: PM activo y CPU, cierre normal
idempotente, caída PM, caída PMG, pérdida de origen seguida de caída PM, terminación
de descendientes, protección por último handle del Job, barrera ante fallo de
terminación, rechazo de lanzamiento después de limpiar y errores de parámetros/
colisiones/ejecutables inexistentes. Algunos grupos reúnen varias comprobaciones.
El detalle exacto está en `IMPLEMENTACION_FASE_3.md` y en el código de la suite.

Todas las comprobaciones de ejecución están `blocked` por requerir Windows. Las
regresiones 0B, 1 y 2 están compiladas pero su ejecución también queda `blocked`.
El workflow Windows y `build.ps1` incluyen las cuatro suites y la captura de
resultados nativos nuevos; no se ha afirmado que ese workflow haya corrido.

Para validar en Windows con LLVM-MinGW ya disponible:

```powershell
./build.ps1 -ReportJsonPath reports/rebuild_guard_windows_20261112_run01.json
```

Usar otra ruta si ya existe. El detalle de la suite de Fase 3 se genera en
`build/phase3_windows_<timestamp>.json`. El script de entrega rechaza sobrescribir
un informe existente. No aprobar la fase hasta revisar estos resultados.

## Evidencia, límites y continuidad

Evidencia nueva: `reports/rebuild_guard_cloud_20261112.json`, con copia exacta en
`fase03/rebuild_guard_cloud_20261112.json`. Incluye rama, commit base, hashes de
fuentes cambiados y artefactos, resultados de PE y estados `blocked` de ejecución.
La documentación consolidada de esta entrega está en `fase03/`.

El guard conserva su ejecutable TEMP y el manifiesto después de cerrar; la
finalización completa de temporales y `pmlog` pertenece al CleanupEngine posterior.
No hay recuperación automática de sesiones antiguas. No se promete restauración
tras muerte simultánea de PM y PMG o pérdida eléctrica. La desaparición sintética
del origen no certifica una extracción USB física ni implementa detección de
extracción mientras PM sigue activo.

**No se inició Fase 4 ni ninguna fase posterior.**
