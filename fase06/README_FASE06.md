# Fase 06 — Aprendizaje Ligero (LearningEngine)

Implementación para revisión. [Informe de Windows](../reports/rebuild_learning.json).

## Candidatos y atribución

`LearningEngine::Inspect` lee `CompanyName` y `ProductName` con las API de
versión de Win32. Exige componentes portables de hasta 80 caracteres ASCII y
valores coherentes entre las traducciones disponibles. Propone tres destinos
con el sufijo `<Company>\<Product>`, uno bajo cada raíz admitida
`%APPDATA%`, `%LOCALAPPDATA%` y `HKCU\Software`.

`LearningEngine::Run` recibe el identificador estable de la aplicación y los
candidatos exactos cuya exclusividad ha verificado el llamador. El llamador
debe garantizar la identidad del ejecutable y del recurso resuelto, así como
la ausencia de escritores externos en ese recurso durante la sesión. El motor
deja intactos los candidatos ambiguos y exige que los directorios y claves
padre existan antes de preparar una redirección o un respaldo.

## Ejecución, restauración y datos

- PM inicia PMGuard antes de preparar el anfitrión. Una reserva por compañía
  y producto excluye otro aprendizaje del mismo candidato. PMGuard conserva
  la reserva hasta que termina. La API está prevista para un proceso dedicado.
- JunctionManager registra la línea base y renombra el original a `.pmbkp`
  antes de redirigir la carpeta a un directorio nuevo en
  `Data/apps/<id>/<SessionID>/`. RegistryManager registra la línea base y
  respalda el subárbol existente en el directorio temporal de sesión. El
  manifiesto usa las operaciones de las fases anteriores.
- Al salir el ejecutable, JobManager termina los descendientes que queden
  y espera a que el Job se vacíe. Si la clave exclusiva estaba ausente en
  la línea base y existe al cierre, RegistryManager valida una exportación
  temporal. Las claves preexistentes se restauran y sus valores se quedan
  en el anfitrión.
- PM y PMGuard comparten la restauración. PM restaura el anfitrión antes de
  copiar la exportación validada al directorio portable y guardar
  `Data/recipes/<id>.ini`. La receta añade una sección
  `[Learning.<SessionID>]` y actualiza `[Learning]` → `LatestSession`.
  Las secciones previas y los datos de otras sesiones se conservan.
- En una caída, PMGuard restaura el anfitrión. Los archivos que la aplicación
  ya escribió por las Junctions pueden quedar en el directorio de sesión,
  sin receta. Si falla la publicación, el motor devuelve el error y conserva
  la exportación temporal que haya completado.

## Validación

`build.ps1` compila con el LLVM-MinGW portable y ejecuta la suite sintética
de Fase 6 y las regresiones 0B–5. CMake compila el mismo código y declara
las mismas suites para CTest. GitHub Actions cruza la compilación desde
Linux y ejecuta las pruebas nativas en Windows.

En Windows local pasaron los seis escenarios sintéticos y las regresiones
0B–5. La inspección PE confirmó AMD64 y `SWAPRUN:CD`. El JSON registra el
commit probado, el SHA-256 del binario y el resultado de cada escenario.

Las pruebas usan carpetas sintéticas y un subárbol HKCU con GUID propio.
Las aplicaciones reales, las ACL personalizadas de Registro y la desconexión
física USB quedan pendientes de validación. La atribución de candidatos y
la aplicación posterior de recetas requieren un llamador con evidencia de
exclusividad.
