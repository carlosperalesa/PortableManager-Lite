# Fase 04 — Directory Junctions

La implementación está en `feat/fase04-junctions`, sobre la Fase 3 aprobada y fusionada en `main` como `94ac3f0`. La aprobación y la fusión de esta fase quedan pendientes del propietario.

## Alcance

`JunctionManager::Prepare` recibe una carpeta exclusiva del anfitrión ya atribuida a una aplicación y un directorio existente de datos relativo a `Data/apps/<id>`. El llamador arranca PMGuard antes de preparar la redirección, registra el Job después y usa `CleanupSession` ante un fallo. El módulo comprueba las rutas, rechaza puntos de reanálisis en los antecesores y exige NTFS para el directorio del anfitrión. La unidad de datos de PM puede ser distinta.

El manifiesto de la sesión activa registra `REMOVE_JUNCTION` antes de crear un directorio provisional hermano con GUID. Guarda su identidad de archivo antes de configurar la Junction. Luego verifica el punto de montaje, renombra la carpeta original a `.pmbkp` si existía y publica la Junction con un renombrado sin reemplazo. Tanto PM como PMGuard usan el mismo rollback bajo el mutex de sesión, después de terminar los Jobs.

El rollback verifica el directorio padre, la identidad del objeto, el tag `IO_REPARSE_TAG_MOUNT_POINT` y el destino registrado en el punto de reanálisis. La validación del enlace funciona con el destino desconectado. La implementación usa `SetFileInformationByHandle(FileDispositionInfo)` sobre el mismo handle abierto con `FILE_FLAG_OPEN_REPARSE_POINT`. Así, la validación y la retirada se refieren al mismo objeto, aun si cambia el nombre. Esta es la diferencia de implementación respecto al `RemoveDirectoryW` por ruta que describe el PoC. [Operaciones con puntos de reanálisis](https://learn.microsoft.com/en-us/windows/win32/fileio/reparse-point-operations) · [SetFileInformationByHandle](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileinformationbyhandle)

El respaldo se restaura por identidad y sin reemplazar entradas. Cuando un tercero sustituye una carpeta, un enlace o un respaldo, el manifiesto conserva la operación pendiente. Si PM se interrumpe entre la creación del directorio provisional y el registro de su identidad, queda un posible directorio provisional vacío que se conserva por falta de prueba de propiedad. En ese intervalo, la carpeta original aún no se ha movido. El rollback rechaza entradas antiguas `REMOVE_JUNCTION` sin prueba de propiedad.

La selección automática de carpetas, las aplicaciones reales y el motor de limpieza final quedan para sus fases correspondientes. La pérdida del destino se simula por renombrado dentro de `%TEMP%`. La desconexión física de un USB en esta fase está `not_run`.

## Verificación

```powershell
pwsh -NoProfile -File tools/bootstrap-toolchain.ps1
pwsh -NoProfile -File build.ps1 -ReportJsonPath reports/rebuild_junctions.json
```

El script verifica el PE AMD64, el indicador `SWAPRUN:CD` y las importaciones. Ejecuta las regresiones 0B, 1, 2 y 3, y genera un JSON de Fase 4 con el estado de cada escenario. La suite usa carpetas sintéticas de `%TEMP%` y arranca el ejecutable de PMGuard en las pruebas de ciclo de vida.

- Informe de la compilación y las pruebas nativas [reports/rebuild_junctions.json](../reports/rebuild_junctions.json).
- GitHub Actions adjunta el informe del runner Windows y ejecuta la compilación cruzada en Linux.

Fase 5 pendiente de aprobación del cierre de Fase 4.
