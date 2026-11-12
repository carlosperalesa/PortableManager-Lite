# Resultados de Validación de Aplicaciones Reales — PortableManager Lite

**Fecha:** 11 de noviembre de 2026  
**Alcance:** Consolidación de las pruebas con aplicaciones reales (`D1` FirefoxPortable, `D2` SumatraPDF y `D3` PuTTY) sin repetir las pruebas ya aprobadas.  
**Informe detallado de las sesiones D1 y D3:** [RESULTADOS_D1_D3.md](file:///d:/Data/PM_Lite_FinalValidation/RESULTADOS_D1_D3.md)

---

## 1. Resumen de Clasificación de Aplicaciones Reales

| Aplicación | Prueba | Paquete / Hash SHA-256 | Estado Funcional | Clasificación de Privacidad | Evidencia Principal |
|---|---|---|---|---|---|
| **FirefoxPortable 160.0** | `D1` | `FirefoxPortable_160.0_English.paf.exe` (`99D406A460EF79B1815980A66649D294ABBB82C103F2013192E81B829A88B7DA`, Authenticode `Valid`) | Aprobada (2 sesiones GUI, perfil en USB) | **Amarillo** (requiere receta complementaria para 23 valores HKCU con ruta USB) | [d1_firefoxportable.json](file:///d:/Data/PM_Lite_FinalValidation/reports/d1_firefoxportable.json), [d1_residue_recipe.md](file:///d:/Data/PM_Lite_FinalValidation/reports/d1_residue_recipe.md) |
| **SumatraPDF** | `D2` | *(Validada en fase histórica previa)* | Aprobada (comunicada en historial) | **Verde** | [RESULTADOS_D1_D3.md](file:///d:/Data/PM_Lite_FinalValidation/RESULTADOS_D1_D3.md) |
| **PuTTY 0.85** | `D3` | `putty.zip` (`E0B36E5381ECC78DD06A291BAB25FB9D95BC991D0D864CF2741A5BDE82816668`, `PUTTY.EXE` Release 0.85) | Aprobada (2 sesiones GUI, persistencia y restauración de registro) | **Amarillo** (registro restaurado en Verde; JumpList `CustomDestinations` requiere receta de archivo) | [d3_putty.json](file:///d:/Data/PM_Lite_FinalValidation/reports/d3_putty.json), [d3_recent_file_attribution.json](file:///d:/Data/PM_Lite_FinalValidation/reports/d3_recent_file_attribution.json) |

---

## 2. Integración de Hallazgos de Aplicaciones Reales con las PoC de Fases 1 a 5

1. **`D1` — FirefoxPortable (PAF):**
   - El lanzador oficial `FirefoxPortable.exe` mantuvo el perfil en `E:\PM_POC\App\FirefoxPortable\Data\profile` y persistió correctamente el marcador `PM_POC_TEST` y la preferencia `browser.ctrlTab.sortByRecentlyUsed = true`.
   - Dejó 23 valores nuevos bajo `HKCU\Software\Mozilla\Firefox` (`Default Browser Agent`, `DllPrefetchExperiment`, `Launcher`, `PreXULSkeletonUISettings`) cuyos nombres contienen la ruta del ejecutable en el USB (`E:\PM_POC\App\FirefoxPortable\App\Firefox64\firefox.exe`).
   - Con el motor de transacciones de registro validado en la **Fase 3** (`RegCopyTreeW` / `RegDeleteTreeW`) y las recetas dirigidas de la **Fase 4**, PM puede respaldar y restaurar esas subramas específicas sin interferir con el Firefox instalado en el anfitrión.

2. **`D3` — PuTTY (ZIP):**
   - PuTTY almacena sus sesiones en `HKCU\Software\SimonTatham\PuTTY\Sessions`.
   - Con la arquitectura validada en las **Fases 3 y 5**, PM respalda tanto la clave de la aplicación como la existencia previa del padre (`HKCU\Software\SimonTatham`), persiste las sesiones portables en `Data/apps/putty/registry/` durante el cierre normal y elimina la clave padre si originalmente no existía (`CleanupParentIfEmpty`), además de permitir incluir en la receta la limpieza verificada del archivo JumpList atribuible en `%APPDATA%\Microsoft\Windows\Recent\CustomDestinations`.
