# Continuidad del desarrollo

11 de noviembre de 2026. Validación final ejecutada, sin PM Core, interfaz gráfica, repositorio ni commit. Consultar RESULTADOS_D1_D3.md como informe completo y los JSON de reports/ como evidencia.

## Estado final

- Fase 1: hashes coincidentes, Firefox Authenticode Valid, PuTTY ZIP íntegro y Release 0.85. Ocho tests iniciales aprobados; nueve en regresión final tras añadir inventario amplio de metadatos.
- D1: dos sesiones reales, perfil USB confirmado, marcador PM_POC_TEST exacto y preferencia CtrlTab true persistentes. AMARILLO por 23 valores HKCU nuevos y límites de auditoría. Limpieza no ejecutada.
- D3: dos sesiones reales con PM_POC_TEST/127.0.0.1. Mecanismo genérico de transacciones restauró ausencia original de la clave. AMARILLO global por archivo Recent atribuible y conservado. Sesión de prueba eliminada por diseño al restaurar.
- Procesos finalizados, snapshot RAM liberado, sin restauraciones pendientes. No quedan órdenes por emitir al ayudante D3: las señales .signal son evidencia histórica y no deben reusarse. El helper rechaza señales antiguas; cualquier repetición exige una nueva ejecución planificada, sin borrar evidencia silenciosamente.
- USB GUID aec5fbfe-d7ae-11f1-a523-00090faa0001, serial 048CCF2C, marcador con hash en usb_identity.json; datos conservados. Sin trabajos de almacenamiento ni escrituras/cola en la muestra final, sin usuarios de archivos registrados según Restart Manager. Expulsar mediante Windows y esperar su confirmación.

## Residuos y limitaciones pendientes

Conservar 23 valores Mozilla de ruta USB, archivo %APPDATA%\Microsoft\Windows\Recent\CustomDestinations\9390ee5b658e96e.customDestinations-ms y clave vacía HKCU\Software\SimonTatham de presencia original desconocida. Recetas propuestas en el informe y d1_residue_recipe.md. Ninguna limpieza destructiva está autorizada por el cierre de esta prueba. LocalAppData\Mozilla vacía pasó de presente a ausente. No se demuestra restauración integral del anfitrión. No afirmar restauración de ACL, ausencia global de residuos o exclusión de todos los accesos de terceros.

## Próximo trabajo

Acordar políticas de residuos y restauración antes de producción: captura completa de valores en RAM y recuperación ante caída, directorios vacíos, atribución de cambios concurrentes, persistencia de sesiones PuTTY al USB y alcance de raíces de registro. No integrar raíces genéricas al código futuro sin autorización. El entorno demuestra viabilidad funcional parcial y no certifica PM Lite para producción.

Histórico comunicado por usuario, no verificable con informes originales: 3A/3B/3B.6/A/B/C aprobadas, D1 amarillo, D2 verde, D3 individual verde. No se repitieron A/B/C/D2 ni desconexión física.
