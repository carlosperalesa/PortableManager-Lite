# INFORME DE AUDITORÍA FINAL — FASE 2 (RUNTIME Y MANIFIESTO)

**Fecha:** 11 de noviembre de 2026  
**Alcance:** Revisión y corrección de coherencia arquitectónica, código, manifiesto, ejemplos y pruebas de la Fase 2 antes de autorizar el inicio de la Fase 3 (`JobObjects` y `PMGuard`).  
**Estado Final:** `passed` (`23/23` escenarios en [`tests/test_phase2_runtime.cpp`](tests/test_phase2_runtime.cpp) + compilación y ejecución limpia de [`fase02/ejemplo_uso_runtime.cpp`](fase02/ejemplo_uso_runtime.cpp)).

---

## 1. Orden de Limpieza — `KILL_JOB` (Sección 1)

### Hallazgo previo
En el ejemplo documental anterior (`ejemplo_session_manifest.ini`), `KILL_JOB` aparecía registrado como `op_0001` (primera operación de preparación), lo que en un recorrido estrictamente LIFO habría provocado que `KILL_JOB` se ejecutara en último lugar.

### Corrección implementada
1. **Regla de preparación en [`ManifestManager::RecordOperationIntent`](src/runtime/manifest_manager.cpp#L483-L500):**
   - `KILL_JOB` debe registrarse como la **última operación de preparación**, inmediatamente antes de iniciar la aplicación portable.
   - Si ya existe una operación `KILL_JOB` registrada en el manifiesto y se intenta registrar cualquier operación adicional de preparación (`RESTORE_BKP`, `REMOVE_JUNCTION`, `RESTORE_REG`, `DELETE_DIR`), `RecordOperationIntent` la rechaza con `ERROR_INVALID_PARAMETER`.
2. **Garantía de ejecución en [`ManifestManager::ExecutePendingRollbacks`](src/runtime/manifest_manager.cpp#L604-L624):**
   - Durante el rollback en orden inverso (LIFO), `KILL_JOB` es siempre la **primera operación efectiva** ejecutada, garantizando que todos los procesos de la sesión se cierren antes de eliminar puntos de unión (`REMOVE_JUNCTION`), restaurar respaldos (`.pmbkp` / `.reg`) o borrar directorios temporales (`DELETE_DIR`).
   - `RollbackSummary` registra en `execution_order_ids` y `execution_order_types` el orden exacto ejecutado.
3. **Verificación:** Cubierto en el **Escenario 17** de [`tests/test_phase2_runtime.cpp`](tests/test_phase2_runtime.cpp).

---

## 2. Respaldo del Registro — Archivos `.reg` (Sección 2)

### Hallazgo previo
La validación previa de `RESTORE_REG` asumía únicamente claves hermanas `.pmbkp` en el registro, mientras que el ejemplo documental mostraba un archivo `.reg` en el directorio de sesión.

### Corrección implementada y evaluación técnica
1. **Mecanismo elegido y evaluación:**
   - Se adopta el respaldo mediante archivos `.reg` exportados dentro del directorio de sesión en `%TEMP%\PortableManager\Session\<SessionID>\<nombre>.reg`.
   - Para exportar e importar en Windows sin privilegios administrativos sobre ramas `HKCU\Software\...`, la herramienta estándar del sistema `reg.exe export "<Clave>" "<Archivo.reg>" /y` y `reg.exe import "<Archivo.reg>"` (junto con `RegDeleteTreeW` previo a la importación para evitar fusión de valores nuevos creados por la aplicación portable) ofrece un formato textual estándar (`Windows Registry Editor Version 5.00` en UTF-16LE con BOM), auditable en disco y recuperable por PMGuard incluso si el USB se desconecta.
   - **Limitaciones documentadas del mecanismo `.reg`:**
     - `reg.exe import` fusiona (*merge*) los valores del archivo `.reg` sobre la clave existente sin borrar valores adicionales creados durante la sesión; por ello es **obligatorio** eliminar primero la clave modificada con `RegDeleteTreeW` (solo después de haber validado que el archivo `.reg` existe y está completo) antes de importar el `.reg`.
     - `reg.exe export` no exporta descriptores de seguridad (ACL/DACL) personalizados de subclaves; sin embargo, para configuraciones de aplicaciones de usuario bajo `HKCU\Software\...` heredadas del perfil del usuario actual, los permisos estándar del propietario (`HKCU`) se preservan correctamente.
     - Una exportación interrumpida podría dejar un archivo `.reg` incompleto; por ello se implementó [`ManifestManager::ValidateRegBackupFile`](src/runtime/manifest_manager.cpp#L243-L315).
2. **Validación de integridad del respaldo `.reg` ([`ValidateRegBackupFile`](src/runtime/manifest_manager.cpp#L243-L315)):**
   - Comprueba que la ruta esté dentro de `%TEMP%` y tenga extensión `.reg`.
   - Comprueba que el archivo exista, sea legible, tenga tamaño válido, comience por `Windows Registry Editor Version 5.00` (o `REGEDIT4`), contenga al menos una sección `[HKEY_CURRENT_USER\Software\...]` y termine en un salto de línea completo (`\r\n` o `\n`), rechazando cualquier exportación truncada o incompleta.
3. **Reglas en el manifiesto:**
   - Se registra `BackupPath` apuntando al archivo `.reg` en la sesión, `RegKeyPath` apuntando a `HKCU\Software\...` y `OriginalExisted` (`1` o `0`).
   - `RecordOperationIntent` rechaza `RESTORE_REG` con `OriginalExisted=1` si el archivo `.reg` no supera `ValidateRegBackupFile`.
   - Durante el rollback, el archivo `.reg` **no se elimina** hasta confirmar que la restauración concluyó con éxito. Si la restauración falla, el archivo `.reg` se conserva intacto en la carpeta de sesión y la operación se marca `FAILED`.
4. **Verificación:** Cubierto en los **Escenarios 12, 18 y 19** de [`tests/test_phase2_runtime.cpp`](tests/test_phase2_runtime.cpp) usando exclusivamente claves sintéticas en `HKCU\Software\PortableManager\Tests\...`.

---

## 3. Sesiones Huérfanas (Sección 3)

### Revisión y directiva aplicada
- [`SessionManager::OpenExistingSession`](src/runtime/session_manager.cpp#L118-L127) se utiliza exclusivamente para que PMGuard (`--guard <SessionID> <Handle>`) abra la sesión activa indicada explícitamente por PM.
- [`SessionManager::InitializeNewSession`](src/runtime/session_manager.cpp#L71-L82) **no realiza ningún escaneo, limpieza ni recuperación automática** de sesiones antiguas al iniciar una nueva instancia de PM.
- No se implementa recuperación posterior automática si PM y PMG desaparecen simultáneamente.
- [`SessionManager::DiscoverOrphanSessions`](src/runtime/session_manager.cpp#L129-L176) se mantiene únicamente como consulta diagnóstica pasiva de solo lectura (funcionalidad futura): no se invoca al iniciar PM y jamás ejecuta limpiezas ni restauraciones automáticas.
- **Verificación:** Cubierto en los **Escenarios 20 y 21** de [`tests/test_phase2_runtime.cpp`](tests/test_phase2_runtime.cpp).

---

## 4. Manifiesto `session.ini` sin Dependencia del USB (Sección 4)

### Corrección implementada
- Se eliminó `PmRootDir` / `PmRootPath` y cualquier referencia al destino en el USB (`PortableTargetRef` / `SourcePath`) tanto de [`ManifestManager::SaveToDisk`](src/runtime/manifest_manager.cpp#L350-L386) como de [`fase02/ejemplo_session_manifest.ini`](fase02/ejemplo_session_manifest.ini).
- PMGuard no necesita acceder al USB para restaurar el anfitrión:
  - `REMOVE_JUNCTION` utiliza únicamente `TargetPath` en el anfitrión (`RemoveDirectoryW` elimina el reparse point NTFS local sin acceder al volumen de destino).
  - `RESTORE_BKP` utiliza `TargetPath` y `BackupPath` (`<TargetPath>.pmbkp`) en el anfitrión.
  - `RESTORE_REG` utiliza `RegKeyPath` (`HKCU\Software\...`) y `BackupPath` (`%TEMP%\PortableManager\Session\<SessionID>\*.reg`).
  - `DELETE_DIR` utiliza `TargetPath` en el anfitrión.

---

## 5. Unificación de Estados (Sección 5)

Se unificaron en código, manifiesto y documentación los 5 estados de operación (`OperationState`):
- `INTENT_RECORDED`: Intención persistida en `session.ini` antes de modificar el anfitrión.
- `APPLIED`: Modificación física aplicada en el anfitrión.
- `ROLLED_BACK`: Operación inversa ejecutada y verificada satisfactoriamente.
- `FAILED`: Falló la restauración; los respaldos (`.pmbkp` / `.reg`) se conservan intactos.
- `SKIPPED`: Operación omitida de forma segura porque el cambio físico no llegó a producirse antes de una interrupción.

La sesión solo puede pasar a `CLEANED` cuando **todas** las operaciones han alcanzado un estado terminal aceptable (`ROLLED_BACK` o `SKIPPED`).

---

## 6. Corrección del Ejemplo C++ (Sección 6)

El archivo [`fase02/ejemplo_uso_runtime.cpp`](fase02/ejemplo_uso_runtime.cpp) fue actualizado y compilado con `clang++.exe -std=c++20 -O2 -Wall -Wextra -Werror`:
- Utiliza la API real de `SessionManager` y `ManifestManager`.
- Comprueba todos los resultados (`InitializeNewSession`, `RecordOperationIntent`, `fs::rename`, `UpdateOperationState`, `TransitionState`, `ExecutePendingRollbacks`, `FinalizeSessionAndCleanupOwnFiles`).
- Comprueba `rollbackSummary.failed_count == 0` y no ejecuta `fs::remove_all()` si existen operaciones fallidas o respaldos pendientes.

---

## 7. Organización Documental por Fase (Sección 7)

- Se migró el contenido de `fase2/` a [`fase02/`](fase02) y se eliminó la carpeta antigua `fase2/`.
- Se creó [`fase01/`](fase01) para mantener la convención de dos dígitos (`fase01/` .. `fase08/`) en todo el proyecto.
- Se conservó intacto el informe histórico [`fase02/rebuild_runtime.json`](fase02/rebuild_runtime.json) (y [`reports/rebuild_runtime.json`](reports/rebuild_runtime.json)) y se generó el nuevo informe de auditoría [`fase02/rebuild_runtime_audit.json`](fase02/rebuild_runtime_audit.json) (y [`reports/rebuild_runtime_audit.json`](reports/rebuild_runtime_audit.json)).
