# Fase 05 — Registro Win32

La rama parte de la Fase 4 aprobada y fusionada en [PR #3](https://github.com/carlosperalesa/PortableManager-Lite/pull/3). La Fase 5 queda pendiente de revisión del propietario.

## Preparación y restauración

El llamador identifica una clave exclusiva de una aplicación bajo `HKCU\Software`, arranca PMGuard, llama a `RegistryManager::Prepare` y registra el Job antes de lanzar la aplicación. Para una clave que todavía no existe, su clave padre debe existir. Las claves compartidas, rutas ambiguas y los árboles con enlaces simbólicos quedan fuera del alcance. La atribución debe excluir escritores ajenos y sesiones superpuestas que administren la misma clave.

`Prepare` comprueba la clave y, si existe, usa el `reg.exe` del directorio de sistema para exportarla a un archivo `.reg` único de la sesión en `%TEMP%`. Valida el formato UTF-16LE, el límite de 16 MiB y el alcance de las secciones y registra la clave, la existencia previa, la ruta de respaldo y su SHA-256. La intención queda persistida antes de permitir el lanzamiento. El procedimiento aplica la decisión DT-04 y la sección 3.8 de [`AGENTS.md`](../../AGENTS.md). [Exportación de claves de Windows](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/reg-export)

PM y PMGuard ejecutan el mismo rollback bajo el mutex de sesión. Si falla la terminación de un Job, la restauración queda pendiente. Para una clave original, el rollback valida el respaldo y mantiene un handle de lectura abierto que bloquea escritura y sustitución durante la importación. Después elimina el árbol modificado, importa en la vista nativa de 64 bits y compara una nueva exportación con el SHA-256 original. Si la clave era nueva, elimina únicamente ese árbol. Persiste `ROLLED_BACK` antes de retirar el respaldo. Si falla la restauración conserva el respaldo, y si solo falla retirar el archivo, puede reintentar sin repetir los cambios en el Registro. [Importación de claves de Windows](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/reg-import)

El formato `.reg` conserva valores, tipos y subclaves. Las ACL personalizadas, otros metadatos de seguridad y la vista separada de 32 bits quedan fuera de esta implementación. El archivo temporal puede contener valores de la aplicación y permanece en la sesión si la restauración falla. Una interrupción durante la exportación previa al registro de intención puede dejar un archivo temporal en la sesión, sin cambios en el Registro. El manifiesto guarda el estado operativo y los códigos de error. Los volcados `.reg` quedan excluidos de Git.

## Verificación

```powershell
pwsh -NoProfile -File tools/bootstrap-toolchain.ps1
pwsh -NoProfile -File build.ps1 -ReportJsonPath reports/rebuild_registry.json
```

La suite crea claves sintéticas exclusivas con GUID bajo HKCU, usa un proceso auxiliar propio y comprueba la restauración por PM y por PMGuard tras una caída de PM. El informe de compilación incorpora la suite de Registro y las regresiones 0B a 4. Las pruebas con aplicaciones reales, la prueba física de desconexión USB y el aprendizaje están fuera del alcance de esta fase.

Windows local superó 8/8 escenarios de Registro y las regresiones 0B a 4. La compilación verificó PE AMD64 y `SWAPRUN:CD` con el toolchain oficial. [Informe JSON](../../reports/rebuild_registry.json)
