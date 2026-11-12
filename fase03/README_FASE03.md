# Fase 03 — JobObjects y PMGuard

**Estado:** implementación y pruebas automatizadas Windows `passed` (Fase 3:
9/9 grupos; regresión 0B/1/2, con 23 escenarios en Fase 2). Aprobación humana
pendiente. PR #2 en borrador; Fase 4 no iniciada.

- Alcance, protocolo y límites: [IMPLEMENTACION_FASE_3.md](IMPLEMENTACION_FASE_3.md).
- Informe de entrega: [INFORME_FASE03.md](INFORME_FASE03.md).
- Evidencia cloud: `rebuild_guard_cloud_20261112.json`, copia exacta del informe
  nuevo en `reports/`; no sustituye los informes históricos ni una prueba Windows.
- Suite: `tests/test_phase3_guard.cpp`, registrada en CMake y en `build.ps1`.
- La fuente vigente del estado general sigue siendo la documentación de la raíz.

La validación actual está en [INFORME_FASE03.md](INFORME_FASE03.md) y en
[la ejecución 43130358371](https://github.com/carlosperalesa/PortableManager-Lite/actions/runs/43130358371).
Fuente única de evidencia nueva: `../reports/fase03_actions_43130358371.json`.
Se conservan las ejecuciones fallidas y el JSON cloud inicial como históricos.
La aprobación final sigue siendo responsabilidad del propietario.
