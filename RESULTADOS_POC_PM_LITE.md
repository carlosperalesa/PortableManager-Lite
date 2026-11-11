# Resultados de Pruebas de Concepto (PoC Fases 1 a 5) — PortableManager Lite

**Fecha:** 11 de noviembre de 2026  
**Estado:** Todas las PoC (Fases 1 a 5) completadas y aprobadas (`passed`).

---

## Resumen Ejecutivo

| Fase / PoC | Estado | Informe JSON | Binario compilado (SHA-256) |
|---|---|---|---|
| **Fase 1 — PoC Runtime y PMGuard** | `passed` | [poc_fase1_runtime_pmguard.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase1_runtime_pmguard.json) | `EEEDA8E73D0F0247AE676CBF6798A49C1ABACC819CA9F76E78620E775C99EDFF` |
| **Fase 2 — PoC Directory Junction** | `passed` | [poc_fase2_junction.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase2_junction.json) | `146FB47C419AAAF8DA7FD22C15272FEF0CCBDB7FB1ECA9A465F8DA26E73364EB` |
| **Fase 3 — PoC Registro de Windows** | `passed` | [poc_fase3_registro.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase3_registro.json) | `934C9E54A3495BAEA43F0CB9AA6A5C42A62EEE5D0963D0888DDB7E8F8B57387D` |
| **Fase 4 — PoC Aprendizaje ligero** | `passed` | [poc_fase4_aprendizaje.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase4_aprendizaje.json) | *(Generado y verificado en sesión)* |
| **Fase 5 — Integración (3 cierres y `pmlog`)** | `passed` | [poc_fase5_integracion.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase5_integracion.json) | `52C741A0404FFE3FBD3074D3F4A7EF9839708AE5683D88D63CC136712F87ED10` |

---

## 1. Fase 1 — PoC Runtime y PMGuard (`passed`)

- **Objetivo:** Comprobar que un único ejecutable con modo normal y modo `--guard` permite a PMGuard ejecutar el cierre de emergencia aunque desaparezca el USB, sin consumir CPU por polling, sin ejecutar limpiezas mientras PM sigue activo y evitando duplicación de acciones mediante exclusión mutua.
- **Evidencia:** [poc_fase1_runtime_pmguard.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase1_runtime_pmguard.json)
- **Resultados verificados:**
  1. **Ejecutable único y `/SWAPRUN:CD`:** Compilado `pm_poc.exe` (13.824 bytes) con el bit `IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP` (`0x0400`) en la cabecera PE (`0x102` → `0x502`).
  2. **Liberación de directorio de trabajo y copia a `%TEMP%`:** Al iniciar desde la carpeta de origen, PM cambia su `CurrentDirectory` hacia `%TEMP%\PortableManager\Session\<SessionID>` (`SetCurrentDirectoryW`) y lanza `--guard` desde la copia local en `%TEMP%`, permitiendo renombrar o eliminar la carpeta de origen sin que PMG falle.
  3. **Espera bloqueante (0% CPU):** Durante 1.200 ms de observación con PM activo, PMG permaneció bloqueado en `WaitForSingleObject(hPmProcess, INFINITE)` con **0,0 ms de tiempo de CPU** (`guard_cpu_delta_ms_over_1200ms: 0.0`) y no ejecutó limpiezas mientras PM seguía vivo.
  4. **Exclusión mutua en cierre normal:** Al cerrar PM normalmente, PM adquirió el Mutex (`Local\PM_Session_Mutex_<ID>`), terminó el Job Object, restauró `.pmbkp` y grabó atómicamente `State=CLEANED` (`CleanupCount=1`, `CleanedBy=PM_NORMAL`). Al despertar PMG, adquirió el Mutex, leyó `State=CLEANED` (`GuardObservedAlreadyCleaned=1`) y terminó sin duplicar acciones.
  5. **Recuperación tras caída de PM y pérdida del origen:** Al eliminar la carpeta de origen y matar abruptamente a PM (`taskkill /F`), PMG despertó, abrió el Job Object nombrado (`OpenJobObjectW`), terminó el proceso hijo de prueba (`TerminateJobObject`), eliminó la carpeta de trabajo y restauró `.pmbkp` (o dejó ausente la carpeta cuando originalmente no existía).

---

## 2. Fase 2 — PoC Directory Junction (`passed`)

- **Objetivo:** Implementar y validar la estrategia de Prioridad 3 para aplicaciones ZIP sin copiar perfiles físicamente al anfitrión, conectando `%APPDATA%` al USB (`E:\`, GUID `\\?\Volume{aec5fbfe-d7ae-11f1-a523-00090faa0001}\`, serial `048CCF2C`).
- **Evidencia:** [poc_fase2_junction.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase2_junction.json)
- **Resultados verificados (cuenta estándar sin elevación):**
  1. **Carpeta original existente en `%APPDATA%`:** Se verificó existencia, se renombró a `.pmbkp` con `MoveFileExW`, se creó la Directory Junction (`FSCTL_SET_REPARSE_POINT`, tag `0xA0000003`) apuntando a `\??\E:\PM_POC\Data\...`, se comprobó el destino con `FSCTL_GET_REPARSE_POINT`, se ejecutó la aplicación de prueba leyendo y escribiendo en el USB a través de `%APPDATA%`, se eliminó **únicamente la Junction** con `RemoveDirectoryW` (conservando intactos los datos del USB) y se restauró `.pmbkp`, comprobando coincidencia exacta del hash SHA-256 del archivo original del anfitrión (`96AEBD877373F1C0C6CE0CD6FE6887DB5C9020A44487DC234DD83E1F28C02EC5`).
  2. **Carpeta original inexistente en `%APPDATA%`:** Se creó la Junction apuntando al USB, la aplicación escribió su configuración en el USB, se eliminó la Junction con `RemoveDirectoryW` y `%APPDATA%` quedó nuevamente sin esa carpeta (`host_dir_absent_after_restore: true`).
  3. **Destino desaparecido / Junction huérfana:** Al desaparecer el directorio destino en el USB, `RemoveDirectoryW` abrió el punto de reanálisis (`FILE_OPEN_REPARSE_POINT`) y eliminó limpiamente la Junction huérfana, permitiendo restaurar `.pmbkp` sin errores.

---

## 3. Fase 3 — PoC Registro de Windows (`passed`)

- **Objetivo:** Probar el respaldo y restauración de claves HKCU sintéticas (`HKCU\Software\PM_PoC_Fase3_e6d2dd69`) mediante `RegCopyTreeW` y `RegDeleteTreeW`.
- **Evidencia:** [poc_fase3_registro.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase3_registro.json)
- **Resultados verificados:**
  1. **Clave originalmente inexistente:** Tras crear valores y subclaves durante la sesión, `RegDeleteTreeW` restauró la ausencia original.
  2. **Clave existente con valores (`REG_SZ`, `REG_DWORD`, `REG_BINARY`, `REG_MULTI_SZ`) y subclaves anidadas:** Respaldada con `RegCopyTreeW`, modificada durante la sesión (valores alterados, subclave borrada y nuevos valores/subclaves añadidos) y restaurada ejecutando primero `RegDeleteTreeW` sobre la clave modificada y luego `RegCopyTreeW` desde el respaldo. El respaldo solo se eliminó tras verificar coincidencia exacta.
  3. **Restauración tras caída de PM:** Un segundo proceso independiente (simulando PMGuard) restauró el estado original a partir del respaldo en HKCU después de que el primer proceso terminara sin restaurar.
  4. **Error durante la restauración:** Al simular un fallo durante la restauración, el respaldo `.pmbkp` se conservó intacto (`backup_preserved_intact_when_restore_failed: true`), permitiendo una recuperación posterior limpia.
- **Hallazgo técnico clave:** `RegCopyTreeW` copia también el descriptor de seguridad de la clave origen; si la clave destino se abre solo con `KEY_READ | KEY_WRITE` (sin `WRITE_DAC`), `RegCopyTreeW` falla con `ERROR_ACCESS_DENIED (5)`. Es obligatorio abrir la clave destino con `KEY_ALL_ACCESS` (o incluir `WRITE_DAC`).

---

## 4. Fase 4 — PoC Aprendizaje Ligero (`passed`)

- **Objetivo:** Validar el mecanismo mínimo de aprendizaje en ubicaciones candidatas acotadas sin auditoría global ni borrado de cambios ambiguos.
- **Evidencia:** [poc_fase4_aprendizaje.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase4_aprendizaje.json)
- **Resultados verificados:**
  1. Extracción nativa de `CompanyName` (`PmSynthVendorLabs`), `ProductName` (`PmSynthLearnedApp`) y `FileVersion` (`1.4.2.0`) mediante `version.dll`.
  2. Registro de baseline en ubicaciones candidatas (`%APPDATA%\<Publisher>\<App>`, `%LOCALAPPDATA%\<App>`, `HKCU\Software\<Publisher>\<App>`) y respaldo `.pmbkp` de la carpeta exclusiva preexistente en `%APPDATA%`.
  3. Comparación tras el cierre de la aplicación, cosecha de datos portables hacia `Data/apps/pmsynthlearnedapp/` en el USB y generación de la receta `Data/recipes/pmsynthlearnedapp.ini` **sin ninguna ruta absoluta persistente** (cumpliendo la Regla 11).
  4. Restauración exacta del anfitrión (SHA-256 `AEC273F045A33759327AC5F389D51F0B595FE76AA81D5E1ECFB1F3525EED0EC4`) y preservación intacta de los cambios concurrentes no atribuibles (`unattributable_concurrent_changes_preserved: true`).

---

## 5. Fase 5 — Integración (`passed`)

- **Objetivo:** Integrar todos los componentes aprobados y validar los cierres **Normal**, **Rápido** y **Crítico**, además de la generación de `%TEMP%\pmlog` (sin extensión) ante errores de restauración.
- **Evidencia:** [poc_fase5_integracion.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase5_integracion.json)
- **Resultados verificados:**
  1. **Cierre Normal (`NORMAL`):** Cierre cooperativo, guardado de archivos y registro portable en el USB, eliminación de la Junction, restauración de `.pmbkp` y de la clave HKCU original, y limpieza del manifiesto.
  2. **Cierre Rápido (`FAST`):** Ante una aplicación que no responde al aviso cooperativo dentro del plazo corto (200 ms), terminación forzada mediante `TerminateJobObject` y restauración completa del anfitrión en 438 ms totales.
  3. **Cierre Crítico / Caída de PM (`PMG_CRITICAL`):** Ante caída abrupta de PM (`Environment.FailFast`), PMGuard despertó inmediatamente, terminó el Job Object sin intentar guardar estado volátil al USB, eliminó la Junction y restauró tanto la carpeta `.pmbkp` como la clave HKCU original.
  4. **Residuo/error de restauración y `%TEMP%\pmlog` (`CRITICAL_WITH_PMLOG`):** Al simular un bloqueo de restauración, se conservaron intactos los respaldos (`.pmbkp` en disco y clave `_pmbkp` en HKCU), se creó `%TEMP%\pmlog` (sin extensión, texto plano conteniendo únicamente las rutas y códigos de error) y finalizaron PM y PMGuard. Posteriormente se limpió el escenario de prueba dejando el anfitrión y el USB sin residuos.
