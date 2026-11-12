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
las ejecuciones de GitHub Actions [43130358371](https://github.com/carlosperalesa/PortableManager-Lite/actions/runs/43130358371) (9/9 PASS) y [43147406760](https://github.com/carlosperalesa/PortableManager-Lite/actions/runs/43147406760) (11/11 PASS con detección de pérdida de origen).
Evidencias consolidadas:
- GitHub Actions Windows (`windows-2022`): [`reports/fase03_actions_43130358371.json`](../../reports/fase03_actions_43130358371.json) y [`../reports/fase03_actions_43147406760.json`](../../reports/fase03_actions_43147406760.json).
- Ejecución local inicial Windows 11 desde disco interno (9/9 PASS): [`../reports/fase03_windows_local_20261111.json`](../../reports/fase03_windows_local_20261111.json) y copia en [`fase03_windows_local_20261111.json`](fase03_windows_local_20261111.json).
- Auditoría inicial USB en unidad `E:` (5/9 PASS, 4/9 FAIL por `ERROR_VIRUS_INFECTED = 225` de Windows Defender): [`../reports/fase03_usb_audit_20261111.json`](../../reports/fase03_usb_audit_20261111.json) y copia en [`fase03_usb_audit_20261111.json`](fase03_usb_audit_20261111.json).
- Ejecución local final con detección de pérdida del origen y recuperación durante cierre crítico (11/11 PASS en disco interno + regresiones 0B/1/2 PASS): [`../reports/fase03_windows_final_20261111.json`](../../reports/fase03_windows_final_20261111.json) y copia en [`fase03_windows_final_20261111.json`](fase03_windows_final_20261111.json).
- Auditoría final en pendrive USB `E:\PM_Lite_Fase03_Test\` e investigación forense de Defender (11/11 PASS desde USB `E:`, 0 detecciones de Defender en el binario actualizado): [`../reports/fase03_usb_audit_v2_20261111.json`](../../reports/fase03_usb_audit_v2_20261111.json) y copia en [`fase03_usb_audit_v2_20261111.json`](fase03_usb_audit_v2_20261111.json).
- Primera corrida de desconexión física (`SessionId = 59f825c2-2f0a-488a-85af-96be0aea3a0b`, `failed` porque el cierre del comando `Prepare` mató el árbol de procesos del arnés antes de la extracción física; corregido en `tools/usb_physical_disconnect_test.ps1` mediante `Win32_Process.Create`): [`../reports/fase03_usb_physical_disconnect_20261111.json`](../../reports/fase03_usb_physical_disconnect_20261111.json) y copia en [`fase03_usb_physical_disconnect_20261111.json`](fase03_usb_physical_disconnect_20261111.json).
- Validación física final con extracción manual real del pendrive `E:` (`SessionId = ed12c050-cbf9-4e98-938e-841b03d1c1e5`, **`passed`** — detección de pérdida del origen `OriginLost`, drenado completo del Job, manifiesto `CLEANED` / `ROLLED_BACK`, sin doble limpieza y con supervivencia del proceso ajeno): [`../reports/fase03_usb_physical_disconnect_v2_20261112.json`](../../reports/fase03_usb_physical_disconnect_v2_20261112.json) y copia en [`fase03_usb_physical_disconnect_v2_20261112.json`](fase03_usb_physical_disconnect_v2_20261112.json).

Se conservan las ejecuciones fallidas y el JSON cloud inicial como históricos.
La aprobación final sigue siendo responsabilidad del propietario.
