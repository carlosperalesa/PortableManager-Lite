# Auditoría de Continuidad y Consolidación — PortableManager Lite

**Fecha:** 11 de noviembre de 2026  
**Estado:** Fases 0 a 5 completadas y verificadas.

---

## 1. Inventario del Entorno (`d:\Data`)

| Elemento mencionado en instrucciones | Ubicación real encontrada | Estado y observaciones |
|---|---|---|
| `PMDocs/` | `d:\Data\PMDocs\` | Contiene la documentación normativa más reciente disponible en el disco (`v0.1-base`, `v0.2.1`, `ARQUITECTURA-v1.1`, `README`). |
| `PM_Lite_FinalValidation/` | `d:\Data\PM_Lite_FinalValidation\` | Contiene los informes y scripts de las validaciones reales `D1` y `D3`, más los informes JSON generados en las PoC de Fases 1 a 5. |
| `PortableManager-Lite-Diseno-v0.1.md` | `d:\Data\PortableManager-Lite-Diseno-v0.1.md` y `d:\Data\PMDocs\PortableManager-Lite-Diseno-v0.1-base.md` | Idénticos (394 líneas, 10-11-2026). |
| `PortableManager-Lite-Diseno-v0.2-Continuacion...` | `d:\Data\PortableManager-Lite-Diseno-v0.2-Continuacion.md` | Versión 0.2 en raíz; superada por `d:\Data\PMDocs\PortableManager-Lite-Diseno-v0.2.1.md`. |
| `ARQUITECTURA_PM_LITE_PM_GUARD.md` | `d:\Data\ARQUITECTURA_PM_LITE_PM_GUARD.md` | Versión 1.0 en raíz; superada por `d:\Data\PMDocs\ARQUITECTURA_PM_LITE_PM_GUARD-v1.1.md`. |
| `FirefoxPortable_160.0_English.paf` | `d:\Data\FirefoxPortable_160.0_English.paf.exe` | Paquete original intacto (SHA-256 `99D406A460EF79B1815980A66649D294ABBB82C103F2013192E81B829A88B7DA`, Authenticode `Valid`). |
| `putty/` | `d:\Data\putty.zip` | Paquete ZIP original intacto (SHA-256 `E0B36E5381ECC78DD06A291BAB25FB9D95BC991D0D864CF2741A5BDE82816668`, `PUTTY.EXE` Release 0.85). |

---

## 2. Equivalencia Documental (`v0.2.1` / `v1.1` frente a `v0.2.2` / `v1.2`)

Los documentos `v0.2.2` y `v1.2` no estaban presentes con esa numeración en `d:\Data\PMDocs\`; sus equivalentes en disco son `PortableManager-Lite-Diseno-v0.2.1.md`, `ARQUITECTURA_PM_LITE_PM_GUARD-v1.1.md` y `README_PortableManager_Lite.md`. Las diferencias normativas introducidas por la especificación actual (`v0.2.2` / `v1.2`) y validadas en las PoC de Fases 1 a 5 son:
1. **Prioridad 3 mediante Directory Junctions (`IO_REPARSE_TAG_MOUNT_POINT`):** En lugar de copiar/mover perfiles físicamente entre el USB y el anfitrión como describía `v0.2.1`/`v1.1`, se renombra la carpeta exclusiva original del anfitrión a `.pmbkp` (si existía) y se crea una **Directory Junction** hacia el USB, eliminando únicamente la Junction (`RemoveDirectoryW`) al cerrar y restaurando `.pmbkp`.
2. **Transacciones de registro mediante `RegCopyTreeW` y `RegDeleteTreeW`:** Respaldo y restauración nativa en `advapi32.dll` (requiriendo `KEY_ALL_ACCESS` / `WRITE_DAC` en la clave destino y limpieza previa con `RegDeleteTreeW` antes de restaurar con `RegCopyTreeW`).
3. **Ejecutable único con modo normal y `--guard`:** Combinando copia local a `%TEMP%`, liberación del directorio de trabajo (`SetCurrentDirectoryW`), espera bloqueante (`WaitForSingleObject`), manifiesto atómico (`MoveFileExW`) y Mutex por sesión.
4. **Registro de emergencia `%TEMP%\pmlog` (sin extensión):** Si al cerrar quedan residuos o respaldos que no pueden restaurarse con seguridad, se conservan los respaldos intactos y se escribe `%TEMP%\pmlog` en texto plano solo con rutas y errores.

---

## 3. Estado Consolidado de Todas las Pruebas

| Prueba | Estado anterior | Evidencia | Acción | Estado actual |
|---|---|---|---|---|
| **Preflight paquetes originales (`FirefoxPortable` PAF y `putty.zip`)** | Aprobada | [package_integrity.json](file:///d:/Data/PM_Lite_FinalValidation/reports/package_integrity.json) | No repetir | `passed` |
| **Pruebas unitarias base (`volume`, `audit`, `registry_transaction`)** | Aprobada (9/9) | [unit_tests_final.json](file:///d:/Data/PM_Lite_FinalValidation/reports/unit_tests_final.json) | No repetir | `passed` |
| **D1 — FirefoxPortable 160.0 (2 sesiones GUI y auditoría)** | Aprobada (Amarillo documentado) | [d1_firefoxportable.json](file:///d:/Data/PM_Lite_FinalValidation/reports/d1_firefoxportable.json), [d1_residue_recipe.md](file:///d:/Data/PM_Lite_FinalValidation/reports/d1_residue_recipe.md) | No repetir | `passed` (Amarillo) |
| **D2 — SumatraPDF** | Aprobada (Verde en historial) | [RESULTADOS_D1_D3.md](file:///d:/Data/PM_Lite_FinalValidation/RESULTADOS_D1_D3.md) | No repetir | `passed` (Verde histórico) |
| **D3 — PuTTY 0.85 (2 sesiones GUI y restauración HKCU)** | Aprobada (Verde registro / Amarillo global por `CustomDestinations`) | [d3_putty.json](file:///d:/Data/PM_Lite_FinalValidation/reports/d3_putty.json), [d3_recent_file_attribution.json](file:///d:/Data/PM_Lite_FinalValidation/reports/d3_recent_file_attribution.json) | No repetir | `passed` (Amarillo) |
| **C (histórica) — Desconexión física real del USB** | Aprobada (histórica comunicada) | [RESULTADOS_D1_D3.md](file:///d:/Data/PM_Lite_FinalValidation/RESULTADOS_D1_D3.md) | No repetir | `passed` (Histórica) |
| **Fase 1 — PoC Runtime y PMGuard (`--guard`, `/SWAPRUN:CD`, copia a `%TEMP%`, CWD, espera 0% CPU, manifiesto y mutex)** | Pendiente de evidencia en workspace actual | [poc_fase1_runtime_pmguard.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase1_runtime_pmguard.json) | Ejecutada en Fase 1 | `passed` |
| **Fase 2 — PoC Directory Junction (`.pmbkp` + Junction al USB + `RemoveDirectoryW` + restauración)** | Pendiente | [poc_fase2_junction.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase2_junction.json) | Ejecutada en Fase 2 | `passed` |
| **Fase 3 — PoC Registro de Windows (`RegCopyTreeW` y `RegDeleteTreeW`, caída de PM y error de restauración)** | Pendiente con APIs nativas Win32 | [poc_fase3_registro.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase3_registro.json) | Ejecutada en Fase 3 | `passed` |
| **Fase 4 — PoC Aprendizaje ligero (metadatos PE, candidatos, baseline, `.pmbkp`, receta relativa y restauración)** | Pendiente | [poc_fase4_aprendizaje.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase4_aprendizaje.json) | Ejecutada en Fase 4 | `passed` |
| **Fase 5 — Integración (PM Core + PMG + Job Objects + Junctions + Registro + 3 cierres + `%TEMP%\pmlog`)** | Pendiente | [poc_fase5_integracion.json](file:///d:/Data/PM_Lite_FinalValidation/reports/poc_fase5_integracion.json) | Ejecutada en Fase 5 | `passed` |
