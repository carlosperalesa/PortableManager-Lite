# IMPLEMENTACIÓN FASE 2: RUNTIME Y MANIFIESTO (C++20 NATIVO — AUDITADA)

**Fecha:** 2026-11-11  
**Estado:** `passed` (23/23 escenarios de Runtime superados: 16 base + 7 de auditoría final + regresión completa de Fase 0B y Fase 1)  
**Toolchain:** LLVM-MinGW `20261104` (Clang `23.1.5`, UCRT, Windows `x86_64`)

---

## 1. Módulos Implementados en `src/runtime/`

### 1.1 `SessionManager`
- **Archivos:** [`src/runtime/session_manager.h`](src/runtime/session_manager.h), [`src/runtime/session_manager.cpp`](src/runtime/session_manager.cpp)
- **Responsabilidades:**
  - Generación de `SessionID` único de 128 bits mediante `CoCreateGuid` (`ole32.dll`).
  - Creación y validación estricta de confinamiento del directorio de sesión en `%TEMP%\PortableManager\Session\<SessionID>\` obteniendo `%TEMP%` mediante `GetTempPathW`.
  - Máquina de estados determinista con 4 estados de sesión:
    - `PREPARING`: Sesión creada y `session.ini` inicializado atómicamente.
    - `ACTIVE`: Operaciones de preparación aplicadas (con `KILL_JOB` registrada en último lugar) y aplicación portable en ejecución.
    - `CLEANING`: Restauración y rollback del anfitrión en curso.
    - `CLEANED`: Todas las operaciones han alcanzado un estado terminal aceptable (`ROLLED_BACK` o `SKIPPED`).
  - **Directiva sobre sesiones huérfanas (Sección 3 de Auditoría):**
    - [`OpenExistingSession`](src/runtime/session_manager.cpp#L118-L127) permite que PMGuard (`--guard <SessionID> <Handle>`) abra la sesión activa indicada por PM sin depender del USB.
    - [`InitializeNewSession`](src/runtime/session_manager.cpp#L71-L82) **no realiza recuperación ni limpieza automática** de sesiones antiguas al iniciar una nueva instancia de PM.
    - [`DiscoverOrphanSessions`](src/runtime/session_manager.cpp#L129-L176) es una consulta diagnóstica estrictamente pasiva (funcionalidad futura) que jamás ejecuta limpiezas ni restauraciones automáticas.
  - Bloqueo de transición a `CLEANED` y prohibición de borrado del directorio de sesión si cualquier operación permanece en `INTENT_RECORDED`, `APPLIED` o `FAILED`, o si queda algún respaldo `.reg` sin confirmar.

### 1.2 `ManifestManager`
- **Archivos:** [`src/runtime/manifest_manager.h`](src/runtime/manifest_manager.h), [`src/runtime/manifest_manager.cpp`](src/runtime/manifest_manager.cpp)
- **Archivo gestionado:** `%TEMP%\PortableManager\Session\<SessionID>\session.ini` (sin `PmRootPath` ni rutas del USB).
- **Tipos de operación soportados (`OperationType`):**
  1. `KILL_JOB`
  2. `REMOVE_JUNCTION`
  3. `RESTORE_BKP`
  4. `RESTORE_REG`
  5. `DELETE_DIR`
- **Estados unificados de operación (`OperationState`):**
  - `INTENT_RECORDED` → `APPLIED` → `ROLLED_BACK` | `FAILED` | `SKIPPED`
- **Reglas obligatorias implementadas:**
  1. **Write-Ahead obligatorio:** [`RecordOperationIntent`](src/runtime/manifest_manager.cpp#L483-L553) valida todos los parámetros y persiste `INTENT_RECORDED` en `session.ini` mediante [`AtomicFile::WriteUtf8Text`](src/core/atomic_file.cpp) **antes** de que se ejecute cualquier cambio en el anfitrión.
  2. **Orden de limpieza — `KILL_JOB` primero en LIFO:** `KILL_JOB` se registra como última operación de preparación inmediatamente antes de lanzar la aplicación; `RecordOperationIntent` rechaza registrar operaciones adicionales después de `KILL_JOB`, y [`ExecutePendingRollbacks`](src/runtime/manifest_manager.cpp#L604-L725) garantiza que `KILL_JOB` sea siempre la primera operación efectiva ejecutada durante el rollback.
  3. **Respaldo del registro mediante archivos `.reg`:** Para `RESTORE_REG`, registra `RegKeyPath` (`HKCU\Software\...`), `BackupPath` (archivo `.reg` en el directorio de sesión en `%TEMP%`) y `OriginalExisted`. [`ValidateRegBackupFile`](src/runtime/manifest_manager.cpp#L243-L315) verifica que el archivo `.reg` exista, esté completo, contenga cabecera válida (`Windows Registry Editor Version 5.00`) y termine en salto de línea antes de permitir el registro.
  4. **Preservación de respaldos (`.pmbkp` y `.reg`):** Si la restauración falla, la operación queda en `FAILED` y el respaldo correspondiente (`.pmbkp` o `.reg`) **jamás se elimina**.

---

## 2. Resultados de Pruebas (`tests/test_phase2_runtime.cpp` — 23/23 `PASS`)

| # | Escenario Verificado | Estado |
|---|----------------------|--------|
| 1–7 | Creación de sesión, GUID único, manifiesto en `%TEMP%`, registro de operación, persistencia Write-Ahead, transiciones de estado y relectura tras reinicio en otro proceso | `PASS` |
| 8 | Caída real (`TerminateProcess`) antes de modificar el anfitrión | `PASS` |
| 9 | Caída real tras `INTENT_RECORDED` antes de renombrar (`SKIPPED`) | `PASS` |
| 10 | Caída real tras renombrar a `.pmbkp` antes de marcar `APPLIED` (`ROLLED_BACK`) | `PASS` |
| 11 | Caída real tras crear Junction simulada (eliminación sin tocar destino portable) | `PASS` |
| 12 | Caída real tras modificar registro con respaldo `.reg` en la sesión | `PASS` |
| 13 | Restauración parcial ante archivo bloqueado y recuperación en reintento | `PASS` |
| 14 | Repetición idempotente de `ExecutePendingRollbacks` | `PASS` |
| 15 | Rechazo de manifiestos truncados, corruptos o con rutas de sistema manipuladas | `PASS` |
| 16 | Conservación intacta de `.pmbkp` ante error y rechazo de sobrescritura de `.pmbkp` previo | `PASS` |
| 17 | **(Auditoría)** Orden efectivo de limpieza: `KILL_JOB` registrado último y ejecutado primero en LIFO | `PASS` |
| 18 | **(Auditoría)** Flujo completo de respaldo `.reg` con `reg.exe export/import` sobre clave sintética `HKCU` y rechazo de `.reg` incompleto | `PASS` |
| 19 | **(Auditoría)** Preservación del archivo `.reg` y estado `FAILED` cuando falla la restauración del registro | `PASS` |
| 20 | **(Auditoría)** Apertura de sesión activa por PMG (`OpenExistingSession`) sin `PmRootPath` ni acceso al USB | `PASS` |
| 21 | **(Auditoría)** Ausencia de recuperación o limpieza automática de sesiones antiguas al iniciar PM (`InitializeNewSession`) | `PASS` |
| 22 | **(Auditoría)** Conservación de respaldos (`.pmbkp` y `.reg`) ante errores de restauración | `PASS` |
| 23 | **(Auditoría)** Rechazo de eliminación de sesión o transición a `CLEANED` con operaciones en `INTENT_RECORDED`, `APPLIED` o `FAILED` | `PASS` |
