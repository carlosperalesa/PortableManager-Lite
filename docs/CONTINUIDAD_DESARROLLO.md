# Continuidad del Desarrollo y Evaluación Técnica — PortableManager Lite

**Fecha:** 11 de noviembre de 2026  
**Estado:** Fases 0 a 5 de validación técnica completadas y aprobadas (`passed`). Listo para iniciar la implementación modular de PM Lite una vez autorizada.

---

## 1. Documentos y Evidencias del Proyecto

- **Auditoría e inventario inicial:** [AUDITORIA_CONTINUIDAD.md](AUDITORIA_CONTINUIDAD.md)
- **Plan de PoC:** [PLAN_POC_PM_LITE.md](PLAN_POC_PM_LITE.md)
- **Resultados de PoC (Fases 1 a 5):** [RESULTADOS_POC_PM_LITE.md](RESULTADOS_POC_PM_LITE.md)
- **Resultados con aplicaciones reales (D1, D2, D3):** [RESULTADOS_APLICACIONES_REALES.md](RESULTADOS_APLICACIONES_REALES.md) y [RESULTADOS_D1_D3.md](../PM_Lite_FinalValidation/RESULTADOS_D1_D3.md)
- **Reportes JSON independientes de cada PoC:**
  - Fase 1 (Runtime y PMGuard): [poc_fase1_runtime_pmguard.json](../PM_Lite_FinalValidation/reports/poc_fase1_runtime_pmguard.json)
  - Fase 2 (Directory Junction): [poc_fase2_junction.json](../PM_Lite_FinalValidation/reports/poc_fase2_junction.json)
  - Fase 3 (Registro Win32): [poc_fase3_registro.json](../PM_Lite_FinalValidation/reports/poc_fase3_registro.json)
  - Fase 4 (Aprendizaje ligero): [poc_fase4_aprendizaje.json](../PM_Lite_FinalValidation/reports/poc_fase4_aprendizaje.json)
  - Fase 5 (Integración y 3 cierres): [poc_fase5_integracion.json](../PM_Lite_FinalValidation/reports/poc_fase5_integracion.json)

---

## 2. Evaluación Técnica Final (Sección 15)

### 2.1 Decisiones de arquitectura verificadas con evidencia real
1. **Ejecutable único (`pm.exe`) con modo normal y modo `--guard`:** Verificado en Fase 1 y Fase 5. La copia automática del ejecutable a `%TEMP%\PortableManager\Session\<SessionID>\` combinada con `SetCurrentDirectoryW` hacia `%TEMP%` libera de inmediato el directorio del USB y permite a PMGuard ejecutar la rutina crítica aunque desaparezca el USB.
2. **Espera bloqueante con 0% CPU y exclusión mutua:** Verificado que `WaitForSingleObject(hPmProcess, INFINITE)` consume **0,0 ms de CPU** durante la espera y que el Mutex de sesión (`Local\PM_Session_Mutex_<ID>`) más el manifiesto atómico (`MoveFileExW` con `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`) impiden cualquier duplicación de limpiezas entre PM y PMGuard.
3. **Prioridad 3 para ZIP mediante Directory Junctions (`IO_REPARSE_TAG_MOUNT_POINT`) sin copia física de perfiles al anfitrión:** Verificado en Fase 2 y Fase 5 en cuenta estándar sin privilegios de administrador. El renombrado atómico a `.pmbkp` (`MoveFileExW`), la creación de la Junction (`FSCTL_SET_REPARSE_POINT`) apuntando al USB (`E:\`) y el desmontaje exclusivo mediante `RemoveDirectoryW` (previa comprobación de `FILE_ATTRIBUTE_REPARSE_POINT`) mantienen los datos en el USB y restauran el anfitrión con integridad exacta (verificada por SHA-256), incluso si el destino en el USB desaparece antes de eliminar la Junction.
4. **Transacciones de registro con `RegCopyTreeW` y `RegDeleteTreeW`:** Verificado en Fase 3 y Fase 5 sobre claves HKCU con valores de múltiples tipos y subclaves anidadas, tanto en cierre normal como tras caída de PM y ante fallos simulados de restauración.
5. **Aprendizaje ligero acotado y recetas con rutas relativas:** Verificado en Fase 4 extrayendo metadatos PE (`version.dll`), comparando baseline vs. cierre en candidatos acotados, generando recetas con rutas estrictamente relativas (Regla 11) y conservando intactos los cambios concurrentes no atribuibles.
6. **Tres modos de cierre (Normal, Rápido, Crítico) y registro `%TEMP%\pmlog`:** Verificados en Fase 5, incluyendo la conservación intacta de respaldos `.pmbkp` y la escritura de `%TEMP%\pmlog` (sin extensión, solo rutas y errores) cuando ocurre un fallo de restauración.

### 2.2 Decisiones técnicas que requieren ajuste o precisión en la documentación normativa ([`docs/PMDocs/`](PMDocs/))
1. **Actualizar `docs/PMDocs/` de `v0.2.1` / `v1.1` a `v0.2.2` / `v1.2`:**
   - Sustituir en la sección de Prioridad 3 de `v0.2.1` y `v1.1` la copia/movimiento físico de perfiles al anfitrión por el mecanismo validado de **Directory Junctions (`IO_REPARSE_TAG_MOUNT_POINT`)**.
   - Incorporar la regla del archivo **`%TEMP%\pmlog`** (sin extensión) cuando quedan respaldos o residuos que no pueden restaurarse con seguridad en cierre crítico.
2. **Requisitos obligatorios al usar `RegCopyTreeW` (`advapi32.dll`):**
   - `RegCopyTreeW` copia el descriptor de seguridad de la clave origen; por tanto, la clave destino **debe abrirse/crearse incluyendo el permiso `WRITE_DAC` (`KEY_ALL_ACCESS = 0xF003F`)**, ya que usar únicamente `KEY_READ | KEY_WRITE` falla con `ERROR_ACCESS_DENIED (5)`.
   - Dado que `RegCopyTreeW` fusiona (*merges*) el árbol sin borrar valores nuevos en el destino, la rutina de restauración **debe ejecutar primero `RegDeleteTreeW` sobre la clave sucia** (tras haber verificado que la clave de respaldo `.pmbkp` existe) antes de restaurar con `RegCopyTreeW`.
3. **Rol real de `/SWAPRUN:CD` (`IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP`):**
   - No debe confiarse en `/SWAPRUN:CD` como mecanismo único de supervivencia porque el kernel de Windows lo ignora si el dispositivo USB se presenta como `DRIVE_FIXED` (habitual en pendrives modernos UASP y SSD USB) y no libera el `CurrentDirectory`. La copia del binario a `%TEMP%` + `SetCurrentDirectoryW` es el mecanismo primario obligatorio; `/SWAPRUN:CD` se conserva como defensa complementaria en el enlazado PE.

### 2.3 Resumen de pruebas aprobadas y bloqueadas
- **Pruebas aprobadas (`passed`):**
  - Preflight de paquetes originales (`FirefoxPortable_160.0_English.paf.exe` y `putty.zip`).
  - 9 pruebas unitarias de herramientas base (`volume`, `audit`, `registry_transaction`).
  - Pruebas de aplicaciones reales: `D1` (FirefoxPortable — Amarillo con receta), `D2` (SumatraPDF — Verde histórico), `D3` (PuTTY — Amarillo con receta).
  - **Fase 1:** PoC Runtime y PMGuard (`poc_fase1_runtime_pmguard.json`).
  - **Fase 2:** PoC Directory Junction (`poc_fase2_junction.json`).
  - **Fase 3:** PoC Registro de Windows (`poc_fase3_registro.json`).
  - **Fase 4:** PoC Aprendizaje ligero (`poc_fase4_aprendizaje.json`).
  - **Fase 5:** PoC Integración, tres cierres y `%TEMP%\pmlog` (`poc_fase5_integracion.json`).
- **Pruebas bloqueadas (`blocked`):** Ninguna (`0`). *(Nota: la variante opcional de USB formateado en exFAT en Fase 2 no se ejecutó físicamente porque el único USB conectado estaba formateado en NTFS, quedando verificado a nivel de arquitectura Win32 que el punto de reanálisis se crea en `C:\` NTFS y redirige mediante `STATUS_REPARSE` a cualquier sistema de archivos local montado).*

### 2.4 Componentes listos para implementación y riesgos abiertos
- **Listos para implementación en PM Lite:**
  - Lanzador y ejecutable único PM Core / PMGuard (`--guard`) con auto-copia a `%TEMP%` y liberación de CWD.
  - Gestor de manifiesto atómico en `%TEMP%` y sincronización por Mutex.
  - Gestor de procesos mediante Job Objects nombrados (`CreateJobObjectW` / `OpenJobObjectW` / `TerminateJobObject`).
  - Motor de Prioridad 3 basado en `.pmbkp` + Directory Junctions (`FSCTL_SET_REPARSE_POINT` + `RemoveDirectoryW`).
  - Motor de transacciones de registro HKCU basado en `RegCopyTreeW` y `RegDeleteTreeW`.
  - Motor de aprendizaje ligero por candidatos PE y generador de recetas con rutas relativas.
  - Rutinas de cierre Normal, Rápido y Crítico con soporte de `%TEMP%\pmlog`.
- **Riesgos técnicos que permanecen abiertos (inherentes a ejecución como usuario estándar en Windows):**
  1. Procesos que escapen del Job Object por creación indirecta fuera del árbol de procesos (p. ej. servidores COM/DCOM lanzados vía `svchost.exe` o delegación a instancias previas de `explorer.exe`).
  2. Rastros globales de Windows fuera de las carpetas de la aplicación (como archivos JumpList en `%APPDATA%\Microsoft\Windows\Recent\CustomDestinations`, Prefetch o cachés del Explorer), que requieren adaptadores específicos por receta si se desea limpiarlos.
  3. Corte eléctrico total o terminación simultánea de PM y PMGuard antes de que cualquiera de los dos pueda ejecutar la rutina de restauración (en cuyo caso los respaldos `.pmbkp` permanecen intactos en el anfitrión sin pérdida de datos originales).
