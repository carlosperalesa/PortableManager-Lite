# Fase 02 — Runtime y Manifiesto (`SessionManager` y `ManifestManager`)

**Estado:** `passed` (23/23 escenarios validados: 16 base + 7 de auditoría final de Fase 2).  
**Toolchain:** LLVM-MinGW `20261104` (Clang `23.1.5`, UCRT, Windows `x86_64`).

---

## Contenido de la Carpeta `fase02/`

Esta carpeta consolida toda la documentación técnica, evidencias JSON y ejemplos verificables de la **Fase 2 (Runtime y Manifiesto)** siguiendo la convención de dos dígitos (`fase01/` .. `fase08/`):

1. [`README_FASE02.md`](file:///d:/Data/fase02/README_FASE02.md) — Índice descriptivo de la carpeta `fase02/`.
2. [`IMPLEMENTACION_FASE_2.md`](file:///d:/Data/fase02/IMPLEMENTACION_FASE_2.md) — Especificación e implementación detallada de [`SessionManager`](file:///d:/Data/src/runtime/session_manager.h) y [`ManifestManager`](file:///d:/Data/src/runtime/manifest_manager.h).
3. [`INFORME_DE_AUDITORIA.md`](file:///d:/Data/fase02/INFORME_DE_AUDITORIA.md) — Informe detallado de la Auditoría Final de Fase 2 (orden de `KILL_JOB`, estrategia `.reg`, sesiones huérfanas, manifiesto sin dependencia del USB, estados unificados y pruebas adicionales).
4. [`rebuild_runtime.json`](file:///d:/Data/fase02/rebuild_runtime.json) — Copia histórica intacta del primer informe JSON de Fase 2 (16 escenarios).
5. [`rebuild_runtime_audit.json`](file:///d:/Data/fase02/rebuild_runtime_audit.json) — Informe JSON resultante de la Auditoría Final de Fase 2 (23 escenarios).
6. [`ejemplo_session_manifest.ini`](file:///d:/Data/fase02/ejemplo_session_manifest.ini) — Ejemplo completo y válido de manifiesto `session.ini` sin `PmRootPath`, con respaldo `.reg` en `%TEMP%` y `KILL_JOB` como última operación de preparación.
7. [`ejemplo_uso_runtime.cpp`](file:///d:/Data/fase02/ejemplo_uso_runtime.cpp) — Ejemplo C++20 compilable sin advertencias (`-Wall -Wextra -Werror`) que comprueba todos los retornos y preserva respaldos ante cualquier fallo.
8. Copias consolidadas de la documentación general al cierre de Fase 2:
   - [`IMPLEMENTACION_PM_LITE.md`](file:///d:/Data/fase02/IMPLEMENTACION_PM_LITE.md)
   - [`PLAN_DE_PRUEBAS.md`](file:///d:/Data/fase02/PLAN_DE_PRUEBAS.md)
   - [`RESULTADOS_RECONSTRUCCION.md`](file:///d:/Data/fase02/RESULTADOS_RECONSTRUCCION.md)
   - [`DECISIONES_TECNICAS.md`](file:///d:/Data/fase02/DECISIONES_TECNICAS.md)
