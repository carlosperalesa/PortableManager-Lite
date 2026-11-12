# Informe de corrección y validación — Fase 03

Rama: `feat/fase03-jobobjects-pmguard`. PR: [#2, en borrador](https://github.com/carlosperalesa/PortableManager-Lite/pull/2), contra `main`.

**Estado vigente:** implementación y pruebas automatizadas `passed`; aprobación
humana pendiente. No se hizo merge ni se inició Fase 4. La entrega cloud inicial
está conservada en `reports/rebuild_guard_cloud_20261112.json`; sus estados
`blocked` describen aquella sesión, no el resultado Windows actual.

## Ejecuciones consultadas y conservadas

| Actions | Commit de rama | Resultado y evidencia |
|---|---|---|
| [43126954771](https://github.com/carlosperalesa/PortableManager-Lite/actions/runs/43126954771) | `34b665e` | Linux PASS; Windows compila; regresiones 0B/1/2 PASS; Fase 3: 4 PASS, 5 FAIL. |
| [43129753101](https://github.com/carlosperalesa/PortableManager-Lite/actions/runs/43129753101) | `bfab116` | Diagnósticos por paso: cuatro grupos fallan con 4 procesos frente a 2; pérdida de origen falla por timeout del evento `_tree`. |
| [43130025527](https://github.com/carlosperalesa/PortableManager-Lite/actions/runs/43130025527) | `08a4f43` | Inventario independiente de cada uno de los cinco grupos: dos auxiliares y dos `conhost.exe`; todos fallan el recuento exacto. |
| [43130358371](https://github.com/carlosperalesa/PortableManager-Lite/actions/runs/43130358371) | `46ab7b9` | Linux PASS; compilación nativa PASS; regresiones 0B/1/2 PASS; Fase 3: **9/9 PASS**. |

Los logs y artefactos se consultaron directamente desde GitHub. El archivo de
artefactos de la ejecución original tiene SHA-256
`496bd1cce0f957bb2b07ac8ecc877df151d3f79528c8ad5783498baf25866a73`.
Cada ejecución tiene un JSON nuevo `reports/fase03_actions_<run_id>.json` con
metadatos y resultados extraídos de sus artefactos, sin sobrescribir informes.
Los dos diagnósticos intermedios conservan también las líneas por paso y los
registros de los controladores; el último conserva el agregado Windows completo.

## Causa raíz por escenario

El informe original no identificaba el primer paso fallido; no permitía atribuir
una causa a los cinco grupos solo por compartir código. Se instrumentaron primero
y se enumeraron después los PID y nombres de imagen dentro de cada Job.

| Escenario fallido | Causa reproducida y comprobada | Corrección |
|---|---|---|
| `normal_once_active_guard_blocked_cpu_children_foreign` | `active_processes_before`: 4, esperado 2. Inventario: dos `test_phase3_guard.exe` y dos `conhost.exe`. | Auxiliares sintéticos sin consola; se conserva la aserción exacta de 2 procesos. |
| `abrupt_pm_death_children_terminated` | Mismo paso y composición, comprobados en su propio Job; no era todavía un fallo de limpieza crítica. | Misma configuración sintética; después se verifica la caída real PM y la limpieza del guard. |
| `guard_death_pm_still_responsible` | Mismo paso y composición en su propio Job; no se había alcanzado la terminación intencional del guard. | Misma configuración sintética; después se verifica el cierre a cargo de PM. |
| `source_folder_loss_then_pm_crash` | Además del inventario de 4 procesos reproducido en 43130025527, 43129753101 registra `tree_ready=258` (`WAIT_TIMEOUT`). El evento podía desaparecer al cerrar su último handle antes de abrirlo el controlador. | Modo sintético sin consola y creación/retención del evento antes de lanzar el árbol. |
| `simultaneous_children_exit` | Mismo paso y composición en su propio Job; no se había llegado a la terminación simultánea. | Misma configuración sintética; después se verifica que PM/PMG sobreviven y el Job queda vacío. |

`CREATE_NO_WINDOW` oculta una consola pero puede crear su `conhost.exe`, que también
queda dentro del Job. La prueba confundía dos auxiliares con dos procesos totales.
No se cambió el resultado esperado a cuatro ni se filtraron hosts para declarar
vacío un Job que todavía tuviera procesos. En su lugar, los auxiliares usan
`DETACHED_PROCESS`, que no asigna consola; el recuento sigue siendo 2 antes y 0 después.

## Correcciones y archivos modificados en esta revisión

- `src/jobs/job_manager.h` y `.cpp`: opción tipada `ConsoleMode::Detached` para
  procesos que no requieren consola. El valor predeterminado `Hidden` conserva
  `CREATE_NO_WINDOW` para los llamadores existentes. Se mantienen creación
  suspendida, asignación al Job antes de reanudar, ausencia de breakaway y
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. No se aceptan flags arbitrarios del llamador.
- `tests/test_phase3_guard.cpp`: auxiliares sin consola, evento `_tree` retenido
  antes de lanzar, diagnóstico de cada paso con valor observado/esperado y último
  error Win32, inventario de miembros del Job, códigos de salida y snapshots de
  manifiesto/controlador anteriores a la contención de errores. Los artefactos
  de CI recogen estos archivos. No se relajan umbrales ni estados esperados.
- `AGENTS.md`: se añade el PATH autorizado sin sustituir directivas y se documentan
  `clang++`, `x86_64-w64-mingw32-clang++`, `llvm-readobj`, `cmake` y `ninja`. Se comprobó
  que el wrapper prefijado tiene target Windows y `clang++` sin target es host Linux.
- Documentación vigente actualizada: `README.md`, `IMPLEMENTACION_PM_LITE.md`,
  `RESULTADOS_RECONSTRUCCION.md`, `PLAN_DE_PRUEBAS.md`, `docs/CODEX_CLOUD.md` y
  `fase03/{README_FASE03.md,IMPLEMENTACION_FASE_3.md,INFORME_FASE03.md}`.
- Evidencia nueva: cuatro archivos `reports/fase03_actions_<run_id>.json`; la
  documentación de `fase03/` enlaza esos informes como fuente única.

También se inspeccionaron `guard_process.cpp`, `guard_entry.cpp`,
`manifest_manager.cpp` y `session_lock.h`. Los fallos reproducidos no justificaron
cambiar su protocolo: el guard sigue esperando bloqueado la muerte PM; la limpieza
común sigue protegida por mutex y recarga de manifiesto. La terminación se verifica
con `ActiveProcesses == 0` antes de restaurar y el rollback se detiene si falla
KILL_JOB. No se incorporaron dependencias ni recuperación de sesiones antiguas.

## Validación real obtenida

Comando ejecutado en Codex Cloud antes de publicar la corrección:

```bash
export PATH="/workspace/.pm-cloud-tools/bin:/workspace/PortableManager-Lite/tools/toolchains/llvm-mingw-linux/bin:$PATH"
bash tools/build-codex-cloud.sh
```

Resultado: código 0, `CROSS_COMPILE_PASSED`, sin errores ni advertencias bajo
`-Wall -Wextra -Werror`, con C++20 y las opciones base aprobadas. No se reinstalaron
herramientas locales ni se ejecutó ningún PE en Linux.

En Windows Server 2022, el workflow ejecutó `build.ps1` con el LLVM-MinGW fijado.
Resultados del commit de código **`46ab7b90257d485de1e93aa85140f5e1451e8529`**:

| Validación | Resultado |
|---|---|
| Compilación cruzada e inspección PE Linux | PASS |
| Compilación nativa Windows e inspección PE | PASS |
| Fase 0B | PASS |
| Fase 1 | PASS |
| Fase 2 | PASS, 23 escenarios |
| Fase 3 | PASS, nueve grupos |

En los cinco ciclos reales se observaron dos procesos iniciales, cero finales,
`CLEANED` persistido y supervivencia del proceso sintético ajeno. La medición del
guard fue de 0 unidades de CPU (100 ns) durante 1.200 ms en cada ciclo; el umbral
permanece en 200.000 unidades (20 ms). Esto no extrapola una garantía de CPU a
cualquier equipo o carga. También pasaron los otros cuatro grupos: cierre del
último handle, barrera ante fallo de terminación, rechazo de lanzamiento tras
limpiar y errores de parámetros/colisiones/ejecutables inexistentes.

Artefacto Windows validado: `13905376318`, SHA-256 de archivo ZIP
`07fc9f0f5b4efafbeb4c55104c19f8c09548c77c5a162d43da4643c8e838cdd0`.
Evidencia consolidada nueva: `reports/fase03_actions_43130358371.json`, que incluye
resultados nativos, hashes de fuentes/PM y diagnóstico completo. Las actualizaciones
de informe posteriores al commit de código no alteran los fuentes validados.

## Límites y revisión pendiente

No se hizo desconexión física USB: la prueba elimina únicamente el ejecutable y
la carpeta de origen sintéticos. No se probaron aplicaciones reales, perfiles de
usuario ni claves reales. La terminación simultánea de PM y PMG y el corte de
energía no tienen recuperación garantizada. Permanecen los límites de finalización
completa de temporales y `pmlog` reservados al CleanupEngine posterior.

El PR #2 permanece en borrador para revisión final. Las pruebas automatizadas
pasaron; **la Fase 3 no se marca aprobada automáticamente**. `main` no se modificó
y no se inició Fase 4.
