# Implementación de Fase 3 — JobObjects y PMGuard

## Alcance autorizado

Se implementan los módulos nativos de Fase 3 descritos en `AGENTS.md`,
`ARQUITECTURA_NATIVE.md` y `PLAN_DE_PRUEBAS.md`. No se implementan Junctions,
Registro, aprendizaje, CleanupEngine ni gestión de aplicaciones (fases 4–8).
Las PoC antiguas son antecedentes, no pruebas de esta implementación.

## JobManager

`src/jobs/job_manager.{h,cpp}` crea un Job no heredable por instancia de
aplicación, con `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, sin breakaway. El nombre
es `Local\PM_Job_<longitudSessionID>_<SessionID>_<InstanceID>`; la longitud
separa inequívocamente los identificadores aunque contengan guiones bajos.
El llamador debe proporcionar un InstanceID único por instancia.

Se reserva un Job vacío con `CreateJobObjectW` y se rechaza `ERROR_ALREADY_EXISTS`
sin registrar una operación que pudiera matar un Job ajeno. Después se persiste
`KILL_JOB` como última preparación, antes de configurar límites, asignar o ejecutar
procesos. Una caída durante la reserva solo puede dejar un Job vacío, que Windows
libera al cerrar su último handle. No se registra información permanente del host.

El proceso se crea con `CREATE_SUSPENDED`, sin herencia de handles, se asigna al
Job y después se reanuda. Si asignar/reanudar falla, se termina exclusivamente
ese proceso suspendido y se cierran sus handles. Los descendientes permanecen en
el mismo Job. Un fallo al lanzar no afecta procesos ajenos.

La preparación, el lanzamiento y la limpieza se serializan con
`Local\PM_Session_Mutex_<SessionID>` (`src/runtime/session_lock.h`). El lanzamiento
recarga el manifiesto bajo el mutex y rechaza sesiones `CLEANING`/`CLEANED` y
operaciones que ya no estén `APPLIED`. Así no se puede lanzar después del cierre.

## PMGuard

`GuardProcess::Start` copia el mismo ejecutable PM indicado por el llamador a
`%TEMP%\PortableManager\Session\<SessionID>\pm-guard.exe`, sin sobrescribir
archivos. No copia configuración, perfiles ni datos de aplicaciones.

El protocolo privado es:

```
pm-guard.exe --guard <SessionID> <PmHandleDecimal> <ReadyEventHandleDecimal>
```

El handle de PM se duplica con `SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION`.
`STARTUPINFOEX` y `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` limitan la herencia al handle
PM y al evento de preparación. No se heredan Jobs, pipes ni otros handles de PM.
El padre espera preparación o salida prematura durante un máximo de 10 segundos.

El guard valida argumentos, identidad de proceso (no puede vigilarse a sí mismo),
apertura de la sesión indicada y cambio de CWD a `%TEMP%`. Señala preparación y
espera `WaitForSingleObject(PM, INFINITE)`. No hace polling, ni enumera sesiones
antiguas, ni limpia mientras PM sigue vivo. El padre puede terminar si el guard
falla: PM conserva la responsabilidad y usa la misma función de limpieza.

Al desaparecer PM, PMG adquiere el mutex, vuelve a cargar `session.ini` y omite
una sesión ya `CLEANED`. Si PM murió con el mutex adquirido, `WAIT_ABANDONED` permite
retomar la inspección real. No necesita el directorio de origen ni el USB.

## Limpieza compartida de Fase 3

`CleanupSession` solo acepta operaciones `KILL_JOB` cuyo nombre coincida exactamente
con la sesión y AppID. Rechaza operaciones de fases futuras sin ejecutar adaptadores
por defecto que pudieran declarar una restauración inexistente. Conserva manifiesto
y respaldos en caso de error.

`TerminateJobObject` termina el grupo y se comprueba `ActiveProcesses == 0` antes de
permitir restauraciones posteriores. La comprobación del drenaje está acotada a
10 segundos; su espera de 10 ms solo se utiliza después de terminar un Job, nunca
para vigilar PM. Un Job ya inexistente se considera terminado de forma idempotente.

En `ManifestManager::ExecutePendingRollbacks` se verifican ahora los resultados
de persistencia. Un fallo de `KILL_JOB` detiene el rollback antes de tocar otros
recursos. Un fallo de escritura impide informar éxito o declarar `CLEANED`.
Las operaciones terminales se omiten en el adaptador de Fase 3. PM, una repetición
de cierre y PMG comparten el estado persistido y el mutex para evitar doble limpieza.

## Integración y compatibilidad

- CMake compila PM, las tres suites anteriores y `test_phase3_guard`.
- `build.ps1` compila/ejecuta las cuatro suites en Windows y recoge JSON real de
  la nueva suite. Rechaza sobrescribir un informe de entrega existente.
- El workflow Windows utiliza este script y conserva las nuevas evidencias.
- Se corrigió el bloqueo de captura del script: lectura asíncrona de ambos pipes
  y espera con timeout antes de recoger el resultado.
- El fallback directo de `build_msvc.ps1`, que omitía módulos existentes, ahora
  exige CMake explícitamente; LLVM-MinGW sigue siendo el toolchain oficial.
- `pm.exe --job-probe` integra el ciclo con un hijo `--smoke-test` del propio PM;
  no lanza aplicaciones de usuario ni inicia Fase 8. La raíz proviene de
  `GetModuleFileNameW`; el CWD se libera a TEMP en ese ciclo.
- El antiguo stub `--guard <SessionID>` ya no devuelve un falso éxito. La suite
  de humo exige código 2 cuando faltan los handles. Las evidencias históricas
  permanecen intactas.

## Detección de pérdida del volumen de origen con PM activo

En `src/guard/guard_process.{h,cpp}` se implementan `IsOriginAccessible` y
`WaitForReleaseOrOriginLoss` conforme a la arquitectura aprobada (`AGENTS.md` §3.5–3.6,
`PMDocs/PortableManager-Lite-Diseno-v0.2.1.md` §23.4 y `v0.2.2` §23.3):

1. `IsOriginAccessible(pm_executable)` verifica la existencia y accesibilidad del
   ejecutable de origen, su directorio contenedor (`GetFileAttributesW`) y la raíz
   del volumen (`GetVolumeInformationW`), sin mantener ningún handle de archivo,
   directorio ni volumen abierto sobre el USB.
2. `WaitForReleaseOrOriginLoss(pm_executable, release_event, timeout_ms, error)`
   crea una ventana popup oculta de nivel superior y registra notificaciones de
   interfaz de volumen (`RegisterDeviceNotificationW` con `DBT_DEVTYP_DEVICEINTERFACE`
   sobre `GUID_DEVINTERFACE_VOLUME`), bloqueándose mediante `MsgWaitForMultipleObjects`
   con tajadas pasivas de 200 ms (~0 % de CPU, sin servicios ni drivers, sin polling
   de alta frecuencia).
3. Si el volumen o ejecutable de origen desaparece mientras PM sigue vivo en memoria
   (gracias a `/SWAPRUN:CD`), `WaitForReleaseOrOriginLoss` devuelve
   `OriginWaitResult::OriginLost` y PM inicia inmediatamente el cierre crítico
   mediante `CleanupSession(session, error)`.
4. `PMGuard` permanece bloqueado en `WaitForSingleObject(parent, INFINITE)` mientras
   PM esté vivo y actúa únicamente si PM desaparece inesperadamente antes o durante
   el cierre crítico (recuperando el mutex incluso con `WAIT_ABANDONED` si PM cayó
   en estado `CLEANING`). Si PM ya completó `CleanupSession` y persistió `CLEANED`,
   `PMGuard` recarga el manifiesto y termina sin ejecutar una segunda limpieza.

## Pruebas preparadas para Windows

La suite crea exclusivamente sesiones GUID y ejecutables auxiliares propios.
Comprueba once grupos:

1. PM activo: guard bloqueado, trabajadores vivos, CPU <=20 ms durante 1,2 s;
   cierre normal y repetición del cierre; un proceso sintético ajeno sobrevive.
2. Caída real del propietario con `TerminateProcess`: PMG termina el árbol y
   persiste `CLEANED`.
3. Caída de PMG: el propietario conserva el trabajo y realiza su cierre.
4. Eliminación de la copia de PM y de su carpeta de origen mientras PM está vivo,
   seguida de su caída; PMG opera desde su copia TEMP.
5. Pérdida simulada del origen mientras PM permanece vivo (`origin_loss_while_pm_alive_triggers_critical_cleanup`):
   PM detecta `OriginLost` sin señal externa, ejecuta el cierre crítico, termina el
   Job (`ActiveProcesses == 0`), persiste `CLEANED`, evita doble limpieza con PMG
   y preserva procesos ajenos.
6. Caída abrupta de PM durante el cierre crítico (`pm_crash_during_critical_cleanup_guard_recovers`):
   PM cae con `TerminateProcess` tras adquirir el mutex de sesión y persistir
   `State=CLEANING`; PMG recupera el mutex (`WAIT_ABANDONED`), termina los procesos
   del Job, persiste `CLEANED` y preserva procesos ajenos.
7. Terminación simultánea de los hijos: PM y PMG permanecen vivos hasta el cierre.
8. Muerte del trabajador al cerrar el último handle del Job.
9. Fallo de terminación inyectado: ninguna restauración posterior; rechazo de
   operaciones futuras; reintento con adaptador explícito controlado por la prueba.
10. Rechazo de lanzamiento después de limpiar la sesión.
11. Identificador inválido, colisión de nombre de Job y ejecutable inexistente.

Los procesos reales del usuario nunca se terminan. Las sesiones fallidas se
preservan para diagnóstico; las exitosas eliminan solo sus archivos conocidos.
La prueba de desaparición es sintética: no demuestra desconexión física USB.
La medición CPU produce valores observados en stdout. En Actions 43130358371 se
registraron 0 unidades de CPU durante 1.200 ms en los cinco ciclos.

## Límites pendientes de validación

Esta sesión Linux solo permite compilación cruzada e inspección de PE. No se
invoca ningún EXE Windows ni se usa Wine como sustituto de las pruebas nativas.
Las pruebas nuevas (9/9 grupos) y las regresiones 0B/1/2 se ejecutaron después
en Actions Windows 43130358371 con resultado `passed`. El bloqueo correspondía
únicamente a la ejecución local Linux, no a esa validación remota.

El guard y el manifiesto TEMP se conservan al terminar el ciclo de Fase 3: no se
implementa un mecanismo de autoeliminación del ejecutable que todavía está corriendo.
La finalización completa de temporales y el registro `pmlog` corresponden al motor
de limpieza posterior. No se promete recuperación tras muerte simultánea de PM y
PMG o corte eléctrico. No se añade detección de desconexión física con PM todavía
vivo; se prueba que el guard sobrevive a la pérdida sintética del origen y la caída PM.

La evidencia Windows ya está disponible; la aprobación de Fase 3 depende de
la revisión final del propietario.
Fase 4 permanece `not_run`.

## Corrección validada en Actions

Ver `INFORME_FASE03.md` para el diagnóstico por escenario y la evidencia nueva.
`ConsoleMode::Detached` permite lanzar los auxiliares sin consola ni conhost;
el modo predeterminado de producción sigue siendo `Hidden`. Las aserciones siguen
exigiendo exactamente dos procesos iniciales y cero al cerrar. El evento `_tree`
se crea y retiene antes del lanzamiento para impedir que desaparezca antes de
que el controlador lo espere. Los diagnósticos de prueba se archivan en CI, sin
introducir logs persistentes en el runtime final de PM.
