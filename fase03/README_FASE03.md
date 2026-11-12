# Fase 03 — JobObjects y PMGuard

**Estado:** implementación realizada; compilación cruzada `passed`; ejecución
Win32 y aprobación de fase `blocked` en esta sesión Linux. No equivale a una
fase aprobada. La autorización de implementación fue dada por el propietario
en la conversación del 12 de noviembre de 2026.

- Alcance, protocolo y límites: [IMPLEMENTACION_FASE_3.md](IMPLEMENTACION_FASE_3.md).
- Informe de entrega: [INFORME_FASE03.md](INFORME_FASE03.md).
- Evidencia cloud: `rebuild_guard_cloud_20261112.json`, copia exacta del informe
  nuevo en `reports/`; no sustituye los informes históricos ni una prueba Windows.
- Suite: `tests/test_phase3_guard.cpp`, registrada en CMake y en `build.ps1`.
- La fuente vigente del estado general sigue siendo la documentación de la raíz.

La Fase 4 no se inició. Se requiere ejecutar la nueva suite y las regresiones
0B, 1 y 2 en Windows antes de evaluar el cierre de Fase 3.
