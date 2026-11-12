# Fase 07 — Motor de limpieza

Implementación en revisión mediante un PR en borrador. La aprobación corresponde al propietario.

## Protocolo de cierre

| Modo | Procesos controlados |
| --- | --- |
| `NORMAL` | Envía `WM_CLOSE` a las ventanas de los procesos del Job. Concede hasta 5 segundos de espera cooperativa cuando hay ventanas notificables. |
| `FAST` | Comparte el plazo cooperativo de 200 ms entre todos los Jobs de la sesión. Agotado el plazo, fuerza la terminación de los procesos restantes. Los 200 ms corresponden a la espera cooperativa. |
| `CRITICAL` | Fuerza la terminación sin aviso cooperativo. PM lo usa ante pérdida del origen y PMGuard lo usa al despertar tras la salida inesperada de PM. |

El mutex de sesión serializa la preparación, el lanzamiento y el cierre. El motor
persiste `CLEANING` antes de intervenir y confirma que los Jobs controlados están
vacíos antes de retirar Junctions o restaurar directorios y HKCU. Si el Job no se
puede terminar o verificar, las restauraciones quedan pendientes. PMGuard usa el
mismo manifiesto local y puede continuar una limpieza interrumpida tras adquirir
un mutex abandonado.

Con PM vivo, la restauración confirmada permite señalar al guard para que
salga y PM retire su copia exacta mediante un handle. En caída abrupta, PMGuard
puede restaurar el anfitrión y persistir `CLEANED` aunque Windows mantenga
mapeado su propio ejecutable. En ese caso conserva la copia y el manifiesto
terminal y registra `REMOVE_GUARD_SHADOW` en `pmlog`. Retirar esa copia después
de que termine PMGuard requeriría un mecanismo adicional al protocolo aprobado.

La receta de aprendizaje describe los recursos observados y las acciones
inversas. Las operaciones ejecutables se toman del manifiesto write-ahead de la
sesión en `%TEMP%`. La copia operativa `.reg` y la carpeta `.pmbkp` sirven para
restaurar el anfitrión. El destino portable de una Junction, los recursos ajenos
y las atribuciones ambiguas quedan fuera de la eliminación automática.

Se conserva el respaldo necesario si falla la restauración. Una vez persistida
la operación inversa, puede reintentarse el retiro de su respaldo sin restaurar
de nuevo el Registro. `%TEMP%\pmlog` se crea únicamente por operaciones pendientes
o residuos de PM. Cada línea contiene la operación, su ruta o clave y un código
Win32. Se conserva cualquier informe anterior y se rechaza escribir sobre un
archivo ajeno o un enlace.

## Verificación

```powershell
pwsh -NoProfile -File build.ps1 -ReportJsonPath reports/rebuild_cleanup.json
```

```bash
bash tools/bootstrap-codex-cloud.sh
bash tools/build-codex-cloud.sh
```

La suite usa ejecutables auxiliares, directorios temporales con GUID y claves
sintéticas de HKCU. Cubre los tres modos, la barrera de Jobs, los conflictos de
propiedad, los reintentos, la pérdida simulada del origen y la recuperación por
PMGuard. GitHub Actions ejecuta también las regresiones 0B–6 en Windows e
inspecciona el PE de Windows x64 compilado en Linux.

**Desconexión física USB:** `not_run`, postergada hasta la validación final por
decisión del propietario. La pérdida de origen se simula renombrando una carpeta
sintética. Evidencia de la fase en [reports/rebuild_cleanup.json](../reports/rebuild_cleanup.json).
