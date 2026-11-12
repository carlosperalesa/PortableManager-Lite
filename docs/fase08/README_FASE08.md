# Fase 08 — Integración nativa

**Estado:** Fase 8 aprobada y fusionada en `main` mediante [PR #9](https://github.com/carlosperalesa/PortableManager-Lite/pull/9), commit `3be874dd62cd592e09fd15c5a37bb6f15166085b`. El [PR #8](https://github.com/carlosperalesa/PortableManager-Lite/pull/8) se cerró sin fusionar porque su contenido está incluido en el PR #9.

Esta es la última fase numerada del plan de reconstrucción. Integra el
catálogo de aplicaciones, instalación, lanzamiento, aprendizaje, cierre,
mantenimiento y la interfaz PortableManager Desktop (PMD) en `pml.exe`.

## Uso

```powershell
.\pml.exe --desktop
.\pml.exe --list
.\pml.exe --register demo App/demo App/demo/demo.exe "Mi aplicación"
.\pml.exe --install demo "C:\paquetes\demo.zip"
.\pml.exe --run demo --argumento
```

`--desktop` abre la píldora. `Ctrl+Espacio` activa la búsqueda y Espacio con
la entrada vacía abre PMD. Las aplicaciones y alias se completan por prefijo.
En búsqueda, el primer `Esc` borra el texto escrito (y en la píldora descarta
la oferta del portapapeles) y el segundo `Esc` contrae la vista a la píldora;
en el escritorio `Esc` retrocede por carpetas hasta volver a la píldora.
`Other/` es la raíz de documentos. El menú de la píldora ofrece `Configuración…`
(crea y abre `Data/config.ini`) y los tres modos de cierre. El menú de una app
permite cerrar, configurar, actualizar, restablecer, archivar y desinstalar.

### Interfaz gráfica de PMD

PMD utiliza una cápsula única expandible renderizada con GDI+ (`UpdateLayeredWindow`
32-bpp ARGB, bordes suavizados y soporte High-DPI por monitor) y menús
contextuales propios del mismo estilo cristal oscuro. En reposo es una
píldora limpia arrastrable con el LED de estado; al detectar un paquete en el
portapapeles se ensancha mostrando el icono de descarga limpio y el selector de
destino (y tras instalarlo exitosamente deja de ofrecerlo hasta que cambie el
portapapeles). Al abrir el buscador o el escritorio, la misma cápsula se
expande desde su centro horizontal hacia los lados y hacia abajo: el buscador
conserva la forma de píldora arriba sin mostrar lista hasta empezar a escribir,
con autocompletado inline y lista de una línea sin subtítulos ni escudo; el
escritorio muestra los iconos directamente sobre el fondo translúcido en
cuadrícula (`Columns` configurable), con el punto de privacidad en la esquina
inferior derecha de cada icono y el escudo de PMG arriba a la derecha.

En `Data/config.ini` bajo `[Desktop]` se configuran `Hotkey` (por defecto
`Ctrl+Space`), `AlwaysOnTop` (`true`/`false`), `Translucent` (`15%`–`100%`, por
defecto `90%`) y `RememberPosition` (`true`/`false`, guardando `X` e `Y` al
cerrar cuando el origen está accesible). Los cambios en `config.ini` se
recargan automáticamente al activar PMD.

## Catálogo y ejecución

Las secciones `[App.<id>]` de `Data/apps.ini` usan `InstallDir`, `Executable`,
`Name`, `Aliases`, `Arguments`, `Type` y `Color`. Se reconocen entradas
anteriores con `Executable` o `Launcher` dentro de `App/<carpeta>/`. El
usuario puede guardar extensiones en `[Associations]` y elegir el navegador
portable con `IsDefaultBrowser=true`. Las secciones `[MagicWord.<id>]` aceptan
`App` y `Names`. Para la acción de respaldo, `[MagicWord._DEFAULT]` acepta
`App`, `URL` y `Argument`, con sustituciones `$I$` y `$W$`. El uso de
aplicaciones del anfitrión desde PMD requiere confirmación.

- PAF se ejecuta mediante su lanzador. Para el resto, `Arguments` configura el
  mecanismo nativo de la app. `Environment=APPDATA,LOCALAPPDATA,USERPROFILE`
  permite elegir variables del proceso hijo que apuntan a datos bajo PML.
- `--learn <id> --approve <recurso> [recursos] [-- argumentos]` requiere
  atribución exclusiva acreditada. `ExclusiveRoaming`, `ExclusiveLocal` y
  `ExclusiveRegistry` en `apps.ini` activan únicamente reglas que coinciden
  exactamente con el wrapper aprendido y los candidatos de la aplicación.
- Al abrir PML se crean un `session.ini` write-ahead y un único PMG, aun
  con cero aplicaciones. Cada lanzamiento tiene su propio Job y registra sus
  operaciones en el mismo manifiesto. El cierre de una app restaura solo su
  alcance. En el cierre global, todos los Jobs se drenan antes del rollback.
- PMG retiene las reservas de las aplicaciones mientras puede necesitarlas
  para la recuperación, y las recibe antes de confirmar que un sustituto
  está listo. Una actualización concurrente devuelve un error de ocupación.
  Si el guardián falta, PML puede iniciar apps que solo requieren un Job.
  El aprendizaje y los nuevos cambios de Junctions o Registro requieren
  que PMG esté operativo antes de preparar los recursos del anfitrión.
- PML supervisa a PMG con una espera asíncrona del pool de Windows. Ante
  caída, conserva las apps y reintenta en la misma sesión. El primer intento
  es inmediato. Los siguientes esperan 2 y 5 segundos tras el intento previo.
  Una caída en los primeros 30 segundos del sustituto consume el presupuesto
  de la misma incidencia. Una vez agotado, el aviso queda rojo hasta reiniciar PML.
- El escudo arriba a la derecha de PMD está verde cuando el guardián está listo.
  El escudo y el LED de la píldora están rojos mientras PMG falta o se recupera.
  El LED también pasa a rojo ante la pérdida del origen y el cierre de
  emergencia. En los demás casos muestra el color habitual de reposo; no hay
  colores adicionales para operaciones en curso o errores, que se informan
  con un diálogo.
  El cierre crítico puede dejar en `pmlog` la copia de PMG aún mapeada por Windows.
- La clasificación inicial es gris. En la primera apertura real desde PMD de
  cualquier aplicación (tanto ZIP como PAF) que aún no tenga `wrapper.ini`, PML
  ejecuta el flujo unificado de aprendizaje (`LearningEngine`) para generar su
  `wrapper.ini` y actualiza automáticamente `Color` en `Data/apps.ini`:
  `Green` si cerró limpia y sin residuos en el anfitrión, `Yellow` si generó
  recursos aprendidos que PML limpió al cerrar, y `Red` si no se pudo restaurar
  el anfitrión o hubo error de rollback. Una aplicación roja pide confirmación
  en PMD y requiere `--allow-red` inmediatamente después del ID en consola.

## Paquetes y mantenimiento

Los comprimidos se inspeccionan con el `tar.exe` del sistema y se extraen en
`App/.temp/<operación>/`. Se validan las rutas, los tipos de entrada, el número
de archivos, el tamaño y el árbol resultante. Si hay varios ejecutables
principales, PMD permite elegir uno. En consola se indica su ruta relativa
dentro del paquete como último argumento de `--install`.

En PAF, PML llama al instalador interactivo con un destino de preparación.
Lee el lanzador declarado en `App/AppInfo/appinfo.ini`. La publicación conserva
el directorio `Data` de una instalación PAF anterior y el `wrapper.ini`
existente, y excluye el homónimo del paquete con cualquier combinación de
mayúsculas. La ejecución del instalador PAF implica confiar en la fuente
elegida. [PortableApps.com](https://portableapps.com/node/62791)

La actualización registra sus movimientos relativos en `session.journal`.
Si falla una publicación se intenta revertir y se conserva la preparación
cuando falla la reversión. Una interrupción del proceso en medio de la
publicación puede requerir revisar el diario y los archivos de `App/.temp`.
No hay una reparación automática de ese escenario.

`--reset`, `--archive` y `--uninstall` requieren `--confirm`. Restablecer
retira datos portables de la app. Archivar conserva identidad, datos y
wrapper. Desinstalar retira la entrada y sus asociaciones. `Other/` queda
fuera de esas operaciones.

## Evidencia

`test_phase8_integration.exe` usa un ejecutable, un perfil y un paquete
sintéticos. Recorre registro, aprendizaje, lanzamiento, recuperación por
caída, actualización, navegación básica en PMD, mantenimiento, sesión compartida con dos Jobs,
reposición de PMG y agotamiento de reintentos. GitHub
Actions ejecuta 0B–8 en Windows y compila para Windows x64 desde Ubuntu.
El JSON combinado se publica como `reports/ci_windows_native.json` en el
artefacto de Windows. La [CI 43298927022](https://github.com/carlosperalesa/PortableManager-Lite/actions/runs/43298927022)
validó el último commit del PR (`96e2e9a`) y la [CI 43299672195](https://github.com/carlosperalesa/PortableManager-Lite/actions/runs/43299672195)
validó el commit de fusión en `main`. Ambas superaron 8/8 escenarios de
Fase 8, las regresiones 0B–7 en Windows y la compilación cruzada desde Linux.
La suite local también pasó completa. La retirada física final queda `not_run`.

Las pruebas con un instalador PAF real y la revisión visual en múltiples
monitores quedan fuera de la suite sintética.
