# AGENTS.md — Documentación (`docs/`)

La fuente única de verdad para las directivas, precedencia y decisiones vigentes de **PortableManager Lite** está en el [`AGENTS.md` de la raíz](../AGENTS.md).

Este archivo se mantiene únicamente para preservar los enlaces internos dentro de `docs/` sin duplicar ni divergir de las reglas maestras.

## Resumen de precedencia

1. **Directiva suprema de seguridad y privacidad del propietario** ([`AGENTS.md` §1 y §2](../AGENTS.md)).
2. **Decisiones operativas vigentes** ([`AGENTS.md` §3](../AGENTS.md)), incluyendo:
   - Aprendizaje y `wrapper.ini` por aplicación (§3.2.1).
   - Persistencia opcional de claves propias del Registro mediante `PersistRegKey=true` en `wrapper.ini` (`Data\apps\<id>\registry\persisted.reg`), opt-in explícito del usuario (por defecto `false`).
   - Recuperación manual de sesiones huérfanas (`--restore-orphans` o desde PMD cuando `DiscoverOrphanSessions()` detecta sesiones sin restaurar).
3. **Documentación funcional y técnica en [`docs/`](README.md)**.
