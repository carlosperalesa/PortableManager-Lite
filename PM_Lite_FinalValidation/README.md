# PM Lite: entorno independiente de validación final

Creado el 11 de noviembre de 2026 en disco interno. Python 3.12 y biblioteca estándar; PowerShell 7 (`pwsh.exe`) para Authenticode y metadatos de versión. No es PM Core ni reconstruye código anterior.

## Archivos

- `scripts/preflight.py`: SHA-256, Authenticode Firefox, CRC ZIP, versión PuTTY mediante lectura de recursos. Extrae únicamente PUTTY.EXE a un directorio temporal propio y lo elimina al terminar; nunca lo ejecuta.
- `scripts/volume.py`: lectura del marcador, GUID y serial; comprobación de identidad antes de operar. No crea marcadores ni busca dispositivos automáticamente.
- `scripts/audit.py`: inventarios de tamaño, fechas y SHA-256; diferencias y errores. Registro: nombres y tipos sin datos de valores. No guarda contenido de archivos ni credenciales. Los snapshots de registro completos se conservan solo en RAM.
- `scripts/registry_transaction.py`: lista exacta de raíces HKCU autorizadas, snapshot RAM, restauración y verificación. Rechaza raíces generales. Requiere confirmar que terminaron los escritores. No captura ni restaura ACL, ni garantiza recuperación ante caída del proceso. Debe mantenerse vivo durante D3; no usar sesiones Python separadas para capturar y restaurar.
- `scripts/firefox_test.py` y `scripts/putty_test.py`: ayudantes para una prueba guiada; no automatizan interfaces ni aprueban resultados. Ejecutarlos directamente solo informa que la prueba está pendiente.
- `tests/`: nueve pruebas sintéticas. `reports/`: evidencia nueva y estados pendientes.

## Ejecución de fase 1

Desde esta carpeta, en PowerShell:

```powershell
python .\scripts\preflight.py
$env:PYTHONPATH = (Resolve-Path .\scripts).Path
python -m unittest discover -s .	ests -v
```

PYTHONPATH solo afecta al proceso de consola, no a la configuración global. La clave sintética usa un hijo UUID bajo HKCU\Software\PM_Lite_FinalValidation_Test y conserva un padre preexistente. No usar información real para tests.

## Controles para las pruebas reales

Requieren confirmación del usuario antes de identificar el USB mediante marcador/GUID/serial. Revalidar identidad antes de cada operación. No sobrescribir instalaciones existentes. No repetir A/B/C/D2. No descargar aplicaciones, editar bases Firefox, cerrar procesos por fuerza ni borrar residuos desconocidos.

Antes de D1 comprobar procesos Firefox del anfitrión, pedir su cierre manual si existen, tomar baseline Mozilla/TEMP/registro y acompañar las dos sesiones GUI. Inventarios con errores o actividad concurrente no prueban limpieza. Nunca abrir un perfil de Firefox en modo escritura para obtener evidencia.

Antes de D3 consultar solo lectura la clave PuTTY; si existe, detenerse sin modificarla. Si está ausente, mantener transacción RAM viva durante ambas sesiones y restaurar después del cierre comprobado. No conectar a servidores. La exclusión de escritores concurrentes es una condición del mecanismo: sin atribución segura, detener la restauración y conservar el proceso/snapshot.

Las pruebas reales y la auditoría de esta ejecución están documentadas en RESULTADOS_D1_D3.md. La expulsión segura siempre se realiza mediante Windows, que efectúa sus comprobaciones finales.


## Cierre de esta ejecución

D1 y D3 realizadas y clasificadas AMARILLO; ver RESULTADOS_D1_D3.md y CONTINUIDAD_DESARROLLO.md. No volver a ejecutar las pruebas reales automáticamente.

Herramientas adicionales: putty_guided_session.py conserva la transacción durante ambas sesiones y usa señales internas; usb_open_files.py consulta Restart Manager sin cerrar aplicaciones. audit.metadata_snapshot inventaría nombres, tamaños y fechas de forma amplia, sin hashes ni contenidos personales en ese modo. Se añadieron evidencias de procesos, baselines, diferencias, residuos y estado USB en reports/. unit_tests.json conserva los ocho tests iniciales; unit_tests_final.json conserva los nueve finales. El helper D3 ya terminó y las señales .signal son históricas, no autorizan otra ejecución. La firma, los perfiles y las auditorías tienen los límites descritos en el informe final. Ningún resultado amarillo se convierte en verde por cerrar la documentación.
