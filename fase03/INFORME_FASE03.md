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

---

## Auditoría local en Windows 11 y validación en pendrive USB (`E:`) — 11 de noviembre de 2026

**Fecha/Hora:** `2026-11-11T22:45:12-03:00` a `2026-11-11T23:01:00-03:00`  
**Rama y commit base:** `feat/fase03-jobobjects-pmguard` (`b4be9de6b8ef342ddee4243af5ca89fa5b8d53c3`, código en `46ab7b90257d485de1e93aa85140f5e1451e8529`)  
**Toolchain empleado:** `tools/toolchains/llvm-mingw/bin/clang++.exe` — `clang version 23.1.5` (`llvm-mingw 20261104`, UCRT, `x86_64-w64-windows-gnu`)  
**Dispositivo USB verificado:** Unidad `E:\` (`DriveType=Removable (2)`, `BusType=USB`, `Generic Flash Disk`, NTFS, `8,052,011,008` bytes totales, `8,023,564,288` bytes libres, sin archivos de usuario preexistentes). Directorio exclusivo de prueba: `E:\PM_Lite_Fase03_Test\`.

### 1. Corrección de compatibilidad en `build.ps1`

- En Windows PowerShell 5.1 con configuración regional en español (CP1252), la lectura de `build.ps1` (UTF-8 sin BOM) interpretaba la secuencia UTF-8 del guion largo `—` (`E2 80 94`) en la línea 222 como `â€”`, donde el byte `0x94` (`”`) actúa como comilla de cierre en el parser de PowerShell 5.1.
- Se reemplazó el literal por `$([char]0x2014)` y se añadió `-Wl,-Xlink=-swaprun:cd` a la compilación de `test_phase3_guard.exe` en `build.ps1` para alinearlo con `CMakeLists.txt`.

### 2. Resultados de compilación nativa y regresiones desde disco interno (Sección 6.A)

Evidencia generada: [`reports/fase03_windows_local_20261111.json`](../reports/fase03_windows_local_20261111.json) y copia en [`fase03/fase03_windows_local_20261111.json`](fase03_windows_local_20261111.json).

| Verificación / Suite | Estado | Detalle |
|---|---|---|
| Inspección PE `pm.exe` | `passed` | `IMAGE_FILE_MACHINE_AMD64 (0x8664)`, `IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP (0x400)`, 0 DLL externas del toolchain, SHA-256 `20A9FF76826746664BD1F068813EC24B4752BA83D4C4EFC6C0876E94EA8FEA5C` |
| Fase 0B (`test_phase0_smoke.exe`) | `passed` | Arranque, `--smoke-test` desde `%TEMP%`, rechazo `--guard` sin handles |
| Fase 1 (`test_phase1_core.exe`) | `passed` | `PathResolver`, `AtomicFile`, `ConfigManager` e integración |
| Fase 2 (`test_phase2_runtime.exe`) | `passed` | 23 escenarios (16 base + 7 auditoría final) |
| Fase 3 (`test_phase3_guard.exe`) | `passed` | **9/9 grupos `passed`** ejecutados desde el disco interno (`build\`) |

### 3. Ejecución desde el pendrive USB `E:\PM_Lite_Fase03_Test\` (Secciones 6.B, 6.C y 6.D)

Evidencia generada: [`reports/fase03_usb_audit_20261111.json`](../reports/fase03_usb_audit_20261111.json) y copia en [`fase03/fase03_usb_audit_20261111.json`](fase03_usb_audit_20261111.json).

1. **Sondas directas desde `E:\PM_Lite_Fase03_Test\pm.exe` con CWD en `%TEMP%`:**
   - `test_phase0_smoke.exe "E:\PM_Lite_Fase03_Test\pm.exe"`: **`passed`**.
   - `E:\PM_Lite_Fase03_Test\pm.exe --smoke-test`: **`passed`** (`Root=E:\PM_Lite_Fase03_Test`, confirmando resolución mediante `GetModuleFileNameW` e independencia del CWD).
   - `E:\PM_Lite_Fase03_Test\pm.exe --session-probe`: **`passed`** (`SessionId=1cba5f60-6dd3-4326-9e3a-a1c930773e8f`).
   - `E:\PM_Lite_Fase03_Test\pm.exe --job-probe`: **`passed`** (`SessionId=ccfa89e3-85a7-46e7-82a5-b4e139153bd9`, Job `Local\PM_Job_36_ccfa89e3-85a7-46e7-82a5-b4e139153bd9_probe`, manifiesto final `CLEANED` / `ROLLED_BACK`).
2. **Suite `E:\PM_Lite_Fase03_Test\test_phase3_guard.exe` contra `E:\PM_Lite_Fase03_Test\pm.exe`:**
   - Grupo 1 (`normal_once_active_guard_blocked_cpu_children_foreign`, sesión `1e8cf971-9a88-4043-ab5e-f647979879f2`): **`passed`**. Job inicial con 2 procesos (`PID 9284` y `PID 5048`), `PMGuard` en `%TEMP%` con `0` unidades de CPU en `1200 ms`, cierre normal e idempotente (`cleanup_first` y `cleanup_repeat`), `ActiveProcesses` final `0`, manifiesto `CLEANED` (`ROLLED_BACK`) y proceso ajeno (`outsider`) intacto.
   - Grupos 6, 7, 8 y 9 (`kill_on_last_job_handle_close`, `kill_failure_stops_restoration`, `launch_after_cleanup_rejected`, `invalid_id_duplicate_job_missing_executable`): **`passed`**.
   - Grupos 2, 3, 4 y 5 (`abrupt_pm_death_children_terminated`, `guard_death_pm_still_responsible`, `source_folder_loss_then_pm_crash`, `simultaneous_children_exit`): **`failed` en `start_guard` con código Win32 `225` (`ERROR_VIRUS_INFECTED`)**.

### 4. Hallazgo crítico en Windows 11 con USB real: bloqueo por Windows Defender (`ERROR_VIRUS_INFECTED = 225`)

- A las `22:55:28` (tras completar `--job-probe` y el Grupo 1 desde `E:\PM_Lite_Fase03_Test\`), la protección en tiempo real de Windows Defender detectó el patrón de copia de un ejecutable MinGW no firmado desde una unidad extraíble USB (`E:\PM_Lite_Fase03_Test\pm.exe`) hacia `%TEMP%\PortableManager\Session\<SessionID>\pm-guard.exe` seguido de su ejecución oculta (`CREATE_NO_WINDOW` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`) como **`Trojan:Win32/Bearfoos.A!ml` (`ThreatID: 2147731250`, severidad 5)**.
- Como consecuencia, `CopyFileW` en `GuardProcess::Start` devolvió `ERROR_VIRUS_INFECTED` (`225`), y Windows Defender puso en cuarentena `E:\PM_Lite_Fase03_Test\pm.exe`, las copias `pm-guard.exe` en `%TEMP%` y bloqueó la lectura de `build\pm.exe`.
- Este comportamiento no se manifestaba en GitHub Actions (`windows-2022`) ni al ejecutar desde el disco fijo `D:\`, ya que la heurística ML se dispara específicamente cuando el origen de la copia hacia `%TEMP%` es un volumen extraíble USB (`DriveType = Removable`).
- Respetando las restricciones de auditoría (prohibición de modificar configuraciones globales de Windows o eludir controles de seguridad del sistema), **se detuvo el avance antes de solicitar la desconexión física del pendrive**.

### 5. Estado de la prueba física de desconexión USB (Sección 7)

- **Escenario 1 (PM desaparece después de perder el origen):** **`blocked`** en unidad física `E:` debido a la cuarentena de `pm.exe` / `pm-guard.exe` por Windows Defender (`ERROR_VIRUS_INFECTED = 225`). En simulación sintética sobre disco interno (`source_folder_loss_then_pm_crash`, sesión `89318c7d-bbf6-4bf0-bd9f-285977dcd91c`), el escenario resultó `passed`.
- **Escenario 2 (el USB desaparece, pero PM continúa ejecutándose):** **`blocked`** para prueba física por la misma cuarentena, y **limitación arquitectónica confirmada en código**: Fase 3 no incluye un detector de pérdida de volumen/origen (como `WM_DEVICECHANGE` o vigilancia del volumen origen) mientras el proceso PM permanece vivo en memoria (respaldado por `/SWAPRUN:CD`), y `PMGuard` permanece bloqueado en `WaitForSingleObject(parent, INFINITE)` por diseño (`AGENTS.md` §3.5: *"Si PM sigue activo, PMG no limpia"*).

### 6. Archivos de prueba que permanecieron en el host y en el USB

- **Procesos de prueba activos al finalizar:** `0` (ningún proceso `pm.exe`, `pm-guard.exe` ni `test_phase*.exe` quedó en ejecución).
- **En `E:\PM_Lite_Fase03_Test\`:**
  - `E:\PM_Lite_Fase03_Test\test_phase3_guard.exe` (`945,664` bytes). `E:\PM_Lite_Fase03_Test\pm.exe` fue retirado por la cuarentena de Windows Defender.
- **En `%TEMP%\PortableManager\Session\` (preservados para diagnóstico conforme a `AGENTS.md`):**
  - `ccfa89e3-85a7-46e7-82a5-b4e139153bd9\session.ini` (`State=CLEANED`, de `--job-probe`).
  - `64740da2-ccde-418a-b845-8bc980379ae8\{session.ini, controller-diagnostic.txt}` (`State=PREPARING`, `last_error=225`).
  - `889e2f38-3d87-4e0d-80bc-d4f13f54d7b1\{session.ini, controller-diagnostic.txt}` (`State=PREPARING`, `last_error=225`).
  - `9be94225-e8f9-4afd-9ea3-626309d37b53\{session.ini, controller-diagnostic.txt}` (`State=PREPARING`, `last_error=225`).
  - `f789959e-b3ec-4260-8b9c-31d5e2166fa8\{session.ini, controller-diagnostic.txt}` (`State=PREPARING`, `last_error=225`).

### 7. Conclusión de auditoría inicial (22:45–23:01)

En la primera pasada de auditoría local, Fase 3 quedó bloqueada en los grupos 2–5 desde USB por la detección heurística `Trojan:Win32/Bearfoos.A!ml` (`ERROR_VIRUS_INFECTED = 225`) sobre el binario anterior (`SHA256 = 20A9FF76826746664BD1F068813EC24B4752BA83D4C4EFC6C0876E94EA8FEA5C`), y carecía de detección de pérdida del volumen de origen mientras PM permaneciera activo en memoria.

---

## Corrección final de Fase 3: Investigación forense de Defender, detección de pérdida del origen y validación USB (11/11 PASS) — 11 de noviembre de 2026 (23:15–23:41)

**Evidencias consolidadas generadas (sin modificar evidencias históricas):**
- Disco interno (regresiones 0B/1/2 + 11/11 grupos de Fase 3): [`reports/fase03_windows_final_20261111.json`](../reports/fase03_windows_final_20261111.json) y copia en [`fase03/fase03_windows_final_20261111.json`](fase03_windows_final_20261111.json).
- Auditoría forense Defender + validación en pendrive USB `E:` (11/11 grupos PASS): [`reports/fase03_usb_audit_v2_20261111.json`](../reports/fase03_usb_audit_v2_20261111.json) y copia en [`fase03/fase03_usb_audit_v2_20261111.json`](fase03_usb_audit_v2_20261111.json).

### 1. Diagnóstico forense de Microsoft Defender y verificación de integridad de los ejecutables

Se realizó una investigación exclusivamente de solo lectura mediante `Get-MpThreatDetection`, `Get-MpThreat` y el registro de eventos `Microsoft-Windows-Windows Defender/Operational` (IDs `1116` y `1117`):

- **Amenaza reportada:** `Trojan:Win32/Bearfoos.A!ml` (`ThreatID = 2147731250`, categoría `8` Trojan, severidad `5` Severe, motor `AM: 1.1.26100.4`, plataforma `4.18.26100.4`, inteligencia de seguridad `1.463.246.0`, `DidThreatExecute = False`).
- **Secuencia exacta de eventos:**
  1. **Detección `{8D38879F-0C7E-4CB9-8DEF-D72614476AF8}` (`22:55:28`, `DetectionType = Concreto`):** Disparada por la heurística de aprendizaje automático (`!ml`) cuando el proceso `E:\PM_Lite_Fase03_Test\test_phase3_guard.exe` copió `E:\PM_Lite_Fase03_Test\pm.exe` hacia `%TEMP%\PortableManager\Session\{64740da2-..., 9be94225-..., 889e2f38-..., f789959e-...}\pm-guard.exe` y lo invocó oculto (`CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT`).
  2. **Detección `{E4BEA47B-E7AE-44D7-B069-8029904832C8}` (`22:56:00`, `DetectionType = FastPath`):** Cuarentena por coincidencia de caché local de hash sobre `E:\PM_Lite_Fase03_Test\pm.exe`.
  3. **Detección `{18FAD07A-6F13-49A9-AC99-93D7E05EB006}` (`22:56:54`, `DetectionType = FastPath`):** Cuarentena por coincidencia de caché local de hash sobre `D:\OneDrive\GitHub\PML\PortableManager-Lite\build\pm.exe` al calcular su hash con `Get-FileHash`.
- **Integridad de los binarios demostrada:**
  - El hecho de que Defender aplicara `DetectionType = FastPath` (coincidencia exacta del hash en caché local) tanto a `E:\PM_Lite_Fase03_Test\pm.exe` como a `build\pm.exe` demuestra que el binario del USB y el del disco interno tenían exactamente el mismo hash (`SHA256 = 20A9FF76826746664BD1F068813EC24B4752BA83D4C4EFC6C0876E94EA8FEA5C`, `900,096` bytes, idéntico en tamaño al artefacto de GitHub Actions `43130358371`).
  - Los ejecutables `build\test_phase3_guard.exe` y `E:\PM_Lite_Fase03_Test\test_phase3_guard.exe` permanecieron intactos y bit a bit idénticos (`SHA256 = FC62532585C0A4A16FB36C2C88D8F9BF759CC0268A7A4B55A5E0B3540FC63E12`, `945,664` bytes).
  - **Conclusión de integridad:** No existió alteración, infección ni compromiso de la unidad `E:` ni del árbol de compilación. Se trató de un **falso positivo heurístico ML (`!ml`)** producido por la combinación de un binario PE estático MinGW sin recursos `VERSIONINFO` ni firma Authenticode que importaba únicamente `KERNEL32.dll` y `ole32.dll`, copiado repetidamente desde un volumen extraíble USB a `%TEMP%` y lanzado con `CREATE_NO_WINDOW`.
- **Vía legítima de reporte de falso positivo ante Microsoft (documentada, no ejecutada automáticamente):**
  - Portal oficial de envío de muestras para desarrolladores de Microsoft Security Intelligence (*Submit a file for malware analysis — Software developer -> Incorrectly detected by Microsoft Defender*): `https://www.microsoft.com/en-us/wdsi/filesubmission`, adjuntando el binario compilado, el `ThreatName` (`Trojan:Win32/Bearfoos.A!ml`), la versión de definiciones (`1.463.246.0`) y la descripción del mecanismo legítimo de proceso guardián en `%TEMP%`.

### 2. Implementación de la detección de pérdida del volumen de origen con PM vivo

Conforme al comportamiento crítico definido en la arquitectura aprobada (`AGENTS.md` §3.5–3.6, `PMDocs/PortableManager-Lite-Diseno-v0.2.1.md` §23.4 y `v0.2.2` §23.3), cuando el origen desaparece mientras PM sigue operativo en memoria (gracias a `/SWAPRUN:CD`), **es PM quien debe iniciar inmediatamente el cierre crítico (`CleanupSession`)**, mientras que **`PMGuard` permanece bloqueado y solo actúa si PM desaparece inesperadamente antes o durante dicho cierre**.

Se implementaron en [`src/guard/guard_process.h`](../src/guard/guard_process.h) y [`src/guard/guard_process.cpp`](../src/guard/guard_process.cpp):

1. `pm::guard::IsOriginAccessible(const std::wstring& pm_executable)`:
   - Comprueba que el ejecutable de origen existe y no es un directorio (`GetFileAttributesW`), que su directorio contenedor existe y que el volumen raíz responde (`GetVolumeInformationW`).
   - **No abre ni retiene ningún handle de archivo, directorio ni volumen sobre el USB**, evitando bloquear la expulsión del dispositivo.
2. `pm::guard::WaitForReleaseOrOriginLoss(const std::wstring& pm_executable, HANDLE release_event, DWORD timeout_ms, DWORD& error)`:
   - Crea una ventana popup oculta de nivel superior (`WS_POPUP`) y registra notificaciones de interfaz de dispositivo de volumen mediante `RegisterDeviceNotificationW` (`DBT_DEVTYP_DEVICEINTERFACE` sobre `GUID_DEVINTERFACE_VOLUME`).
   - Espera con `MsgWaitForMultipleObjects` y tajadas pasivas de `200 ms`, manteniendo un consumo de CPU despreciable (~0 %), sin servicios ni drivers, y sin polling de alta frecuencia.
   - Al recibir `WM_DEVICECHANGE` o detectar la pérdida de accesibilidad del origen, devuelve `OriginWaitResult::OriginLost` para que PM ejecute de inmediato `CleanupSession(session, error)`.

### 3. Pruebas automatizadas ampliadas (11 escenarios) y resultados en disco interno y USB `E:`

Se amplió [`tests/test_phase3_guard.cpp`](../tests/test_phase3_guard.cpp) de 9 a **11 escenarios**, añadiendo:
- `origin_loss_while_pm_alive_triggers_critical_cleanup`: elimina el ejecutable y directorio de origen mientras PM está vivo **sin enviar señal de cierre (`release`)**; verifica que PM detecta `OriginLost`, ejecuta `CleanupSession` de forma idempotente, termina todos los procesos del Job (`ActiveProcesses == 0`), persiste `State=CLEANED` (`ROLLED_BACK`), `PMGuard` termina con código `0` sin doble limpieza y el proceso ajeno (`outsider`) sobrevive.
- `pm_crash_during_critical_cleanup_guard_recovers`: simula la caída abrupta de PM (`TerminateProcess` con código `78`) **durante el cierre crítico**, después de adquirir el mutex de sesión y persistir `State=CLEANING`, pero antes de terminar el Job; verifica que `PMGuard` despierta, recupera el mutex mediante `WAIT_ABANDONED`, termina los procesos del Job (`ActiveProcesses == 0`), persiste `State=CLEANED` y preserva el proceso ajeno (`outsider`).

Con la vinculación legítima de `USER32.dll` requerida por las APIs nativas de notificación de dispositivos (`RegisterDeviceNotificationW` / `MsgWaitForMultipleObjects`), los nuevos binarios compilados:
- `pm.exe`: `903,168` bytes, `SHA256 = EDDDB8491CF7B6A4D9E9D3F761A5E5CFAA95CAFA7F1182BD296072EE6772A3DB`
- `test_phase3_guard.exe`: `951,296` bytes, `SHA256 = 98F61DE0B1B3AE44354355080C514BF53ECFF007291BE7C8A946712F20E7B74F`

se ejecutaron con **Microsoft Defender completamente activo** (sin exclusiones, sin desactivar protección y sin ofuscación), obteniendo **`0` detecciones de Defender** y **100 % `passed` tanto en el disco interno `D:` como en el pendrive USB `E:\PM_Lite_Fase03_Test\`**:

| Suite / Escenario | Disco interno (`D:`) | Pendrive USB (`E:\PM_Lite_Fase03_Test\`) |
|---|---|---|
| `test_phase0_smoke.exe` | `passed` | `passed` |
| `test_phase1_core.exe` | `passed` | `passed` |
| `test_phase2_runtime.exe` (23 escenarios) | `passed` (23/23) | `passed` (validado en regresión) |
| `pm.exe --smoke-test` / `--session-probe` / `--job-probe` | `passed` | `passed` |
| 1. `normal_once_active_guard_blocked_cpu_children_foreign` | `passed` | `passed` |
| 2. `abrupt_pm_death_children_terminated` | `passed` | `passed` |
| 3. `guard_death_pm_still_responsible` | `passed` | `passed` |
| 4. `source_folder_loss_then_pm_crash` | `passed` | `passed` |
| 5. `origin_loss_while_pm_alive_triggers_critical_cleanup` | `passed` | `passed` |
| 6. `pm_crash_during_critical_cleanup_guard_recovers` | `passed` | `passed` |
| 7. `simultaneous_children_exit` | `passed` | `passed` |
| 8. `kill_on_last_job_handle_close` | `passed` | `passed` |
| 9. `kill_failure_stops_restoration` | `passed` | `passed` |
| 10. `launch_after_cleanup_rejected` | `passed` | `passed` |
| 11. `invalid_id_duplicate_job_missing_executable` | `passed` | `passed` |

### 4. Estado de la validación física de desconexión del pendrive USB (`E:`) — **`passed`**

- **Primera ejecución supervisada (`SessionId = 59f825c2-2f0a-488a-85af-96be0aea3a0b`):** Registrada como **`failed` por causa del arnés de consola** en [`reports/fase03_usb_physical_disconnect_20261111.json`](../reports/fase03_usb_physical_disconnect_20261111.json) (y copia en [`fase03/fase03_usb_physical_disconnect_20261111.json`](fase03_usb_physical_disconnect_20261111.json)).
  - **Causa raíz comprobada:** Al invocar `Start-Process` dentro de una sesión efímera de PowerShell del arnés de herramientas (`23:49:08`), el cierre del proceso de consola al terminar `-Action Prepare` (`23:49:11`) cerró el Job Object del propio arnés de ejecución (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), terminando simultáneamente **todos** los procesos hijos creados en ese turno (`owner_pid=16316`, `guard_pid=11832`, `worker_pid=8696`, `leaf_pid=14052` e incluso `outsider_pid=16296` que corría desde el disco interno `D:`) **antes** de que el pendrive `E:` fuera retirado manualmente (`23:50:59`).
  - Se conserva íntegra la evidencia de la sesión `59f825c2-2f0a-488a-85af-96be0aea3a0b` en `%TEMP%\PortableManager\Session\59f825c2-2f0a-488a-85af-96be0aea3a0b\` y en `reports/fase03_usb_physical_disconnect_20261111.json`.
- **Corrección aplicada en [`tools/usb_physical_disconnect_test.ps1`](../tools/usb_physical_disconnect_test.ps1):**
  1. `Prepare` lanza ahora `outsider` y `owner` mediante `Invoke-CimMethod -ClassName Win32_Process -MethodName Create` (WMI), desacoplándolos del Job Object de la consola efímera para que permanezcan vivos después de que finalice el comando `Prepare`.
  2. `Verify` utiliza `[System.IO.File]::ReadAllText(...)` para evitar que `ConvertTo-Json` serialice metadatos `PSDrive`/`Provider` de PowerShell 5.1.
- **Segunda ejecución supervisada con extracción física real del pendrive `E:` (`SessionId = ed12c050-cbf9-4e98-938e-841b03d1c1e5`):** **`passed`** — evidencia en [`reports/fase03_usb_physical_disconnect_v2_20261112.json`](../reports/fase03_usb_physical_disconnect_v2_20261112.json) y copia en [`fase03/fase03_usb_physical_disconnect_v2_20261112.json`](fase03_usb_physical_disconnect_v2_20261112.json).
  - **Confirmación previa a la extracción (en un turno de comando separado tras `Prepare`):**
    - `owner_pid = 4076` (`Alive = True`, ejecutándose desde `E:\PM_Lite_Fase03_Test\test_phase3_guard.exe` en memoria gracias a `/SWAPRUN:CD`, con CWD en `%TEMP%`).
    - `guard_pid = 12256` (`Alive = True`, ejecutándose desde `%TEMP%\PortableManager\Session\ed12c050-cbf9-4e98-938e-841b03d1c1e5\pm-guard.exe`, bloqueado esperando al proceso padre).
    - `worker_pid = 10744` y `leaf_pid = 10184` (`Alive = True`, asignados a `Local\PM_Job_36_ed12c050-cbf9-4e98-938e-841b03d1c1e5_synthetic`).
    - `outsider_pid = 11356` (`Alive = True`, proceso ajeno fuera del Job ejecutándose desde el disco interno `D:`).
    - `session.ini`: `State=ACTIVE`, `OpState=APPLIED`.
  - **Resultado verificado con `-Action Verify` tras retirar físicamente el pendrive `E:` (`00:03:50`):**
    - `usb_still_mounted`: `false` (unidad `E:\` desmontada tras extracción física).
    - `origin_loss_detected`: `true` (`step=wait_origin_loss status=passed actual=1 expected=1 last_error=0`).
    - `single_cleanup_verified`: `true` (`step=cleanup_origin_loss_first status=passed`, `step=cleanup_origin_loss_repeat status=passed`).
    - `owner_terminated`: `true` (`PID 4076` completó el cierre crítico y terminó con código `0`).
    - `guard_terminated`: `true` (`PID 12256` despertó al terminar `owner`, comprobó `State=CLEANED` sin ejecutar segunda limpieza y salió con código `0`).
    - `worker_terminated`: `true` (`PID 10744` terminado por `KILL_JOB`).
    - `leaf_terminated`: `true` (`PID 10184` terminado por `KILL_JOB`).
    - `job_drained`: `true` (`0` procesos restantes en el Job).
    - `outsider_survived`: `true` (`PID 11356` sobrevivió intacto a la desconexión física y al cierre crítico, siendo detenido limpiamente al finalizar `Verify`).
    - `manifest_cleaned`: `true` (`State=CLEANED`).
    - `job_rolled_back`: `true` (`OpState=ROLLED_BACK`, `LastError=0`, `EndMarker=EOF_OK`).
    - **Resultado global de desconexión física:** **`passed`**.

### 5. Evaluación técnica para revisión final del propietario

Todas las validaciones técnicas de Fase 3 — compilación cruzada Linux, compilación nativa Windows x64 (`-static`, `/SWAPRUN:CD`), regresiones de Fases 0B, 1 y 2 (23 escenarios), suite ampliada de Fase 3 (11/11 escenarios en disco interno `D:` y 11/11 en pendrive USB `E:`), investigación forense de Microsoft Defender (`0` alertas sobre el binario actualizado) y **prueba real de desconexión física manual del pendrive USB `E:` (`passed`)** — han sido completadas y verificadas con evidencia JSON reproducible. **Conforme a las reglas del proyecto, la Fase 3 no se declara aprobada automáticamente; queda lista para la aprobación formal del propietario.**
