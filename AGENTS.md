# AGENTS.md --- PortableManager Lite

Este archivo contiene las directivas permanentes para cualquier agente
que trabaje en este repositorio (Codex, Copilot u otro). Debe leerse
antes de inspeccionar, planificar o modificar código.

## 1. Identidad y objetivo del proyecto

**PortableManager Lite (PM)** es un lanzador y gestor portable, ligero y
orientado a la privacidad. Su responsabilidad principal es ejecutar
aplicaciones portables y devolver el anfitrión al estado que tenía antes
de la sesión, eliminando los residuos que PM pueda identificar y
administrar.

PM no es un virtualizador, una sandbox general, un antivirus ni un
auditor exhaustivo de aplicaciones. No debe convertirse en un sistema
pesado.

### Prioridades, en este orden

1.  Privacidad y mínima escritura en el anfitrión.
2.  Restauración del estado previo del anfitrión.
3.  Limpieza de los residuos administrados por PM.
4.  Conservación de los datos propios de las aplicaciones en el
    almacenamiento de PM.
5.  Sencillez, mantenibilidad y bajo consumo de recursos.
6.  Compatibilidad y comodidad de uso.

No sacrificar las prioridades superiores para añadir funciones
secundarias.

## 2. Autoridad y resolución de contradicciones

Antes de cambiar una decisión, consultar la documentación vigente y el
historial de decisiones.

Orden de precedencia:

1.  Última decisión explícita del propietario del proyecto en la
    conversación o en el registro de decisiones.
2.  Directivas de este `AGENTS.md`, siempre que no contradigan una
    decisión posterior del propietario.
3.  Documentación funcional y técnica vigente en [docs/](docs/README.md).
4.  Documentación consolidada de la fase correspondiente.
5.  Informes JSON y evidencias de pruebas.
6.  Documentación histórica o versiones anteriores.

Si dos fuentes vigentes se contradicen y no existe una decisión
posterior que lo resuelva:

-   No elegir silenciosamente una interpretación.
-   No cambiar la arquitectura por iniciativa propia.
-   Documentar la contradicción, sus consecuencias y las alternativas.
-   Detener el trabajo afectado y solicitar decisión.

Los informes históricos demuestran lo que se probó en su momento; no
demuestran por sí solos que la implementación actual siga funcionando.

## 3. Principios arquitectónicos inamovibles

### 3.1 Rutas y portabilidad de PM

-   La raíz de PM es el directorio que contiene `pm.exe`, obtenido
    mediante `GetModuleFileNameW`.
-   Nunca determinar la raíz usando el directorio de trabajo actual
    (CWD).
-   PM debe funcionar igual desde un pendrive, disco externo o carpeta
    de un disco local.
-   Toda ruta persistente de configuración de PM debe ser relativa a la
    raíz de `pm.exe`.
-   No guardar letras de unidad ni rutas absolutas del anfitrión en
    `config.ini` o `apps.ini`.
-   Las rutas absolutas del anfitrión solo se permiten en artefactos
    operativos temporales, como el manifiesto de sesión en `%TEMP%`,
    cuando sean necesarias para restaurar el anfitrión incluso si el
    almacenamiento de PM deja de estar disponible.
-   Validar rutas, normalizar separadores y bloquear escapes de
    directorio no autorizados.

### 3.2 Datos de las aplicaciones

-   Los datos internos de las aplicaciones pertenecen al usuario.
-   La prioridad es mantener esos datos en el almacenamiento de PM.
-   PM no garantiza la integridad de los datos internos de una
    aplicación después de un cierre forzado.
-   PM es responsable de cerrar los procesos que controla y
    limpiar/restaurar el anfitrión; no de corregir el funcionamiento
    interno de cada aplicación.
-   No copiar perfiles de usuario al anfitrión como estrategia de
    portabilidad.
-   No borrar automáticamente datos de aplicaciones por errores de
    ejecución.
-   No diagnosticar ni reparar automáticamente una aplicación que no
    funciona.

### 3.2.1 Aprendizaje y recetas de limpieza (decisión del propietario 2026-11-12)

- El aprendizaje identifica **archivos, carpetas y claves de Registro efectivamente afectados** por una aplicación, para generar recetas de limpieza y restauración; no hace copias de seguridad de la aplicación.
- Las recetas persistentes describen los recursos afectados y las operaciones inversas, con rutas relativas o variables estándar; no contienen datos de configuración de la aplicación ni rutas absolutas del anfitrión.
- Los respaldos `.pmbkp` y `.reg` son **operativos, temporales y destinados exclusivamente a restaurar el anfitrión**. No son un mecanismo de preservación de configuraciones portables.
- Los datos propios de la aplicación se conservan mediante su mecanismo portable o una redirección aprobada. LearningEngine no debe copiar, cosechar ni mover perfiles de aplicaciones para producir recetas.
- En un cierre crítico por desconexión del USB, no escribir al dispositivo ausente; detener los procesos controlados y restaurar el anfitrión a partir del manifiesto y respaldos operativos accesibles en `%TEMP%`.
- Un candidato inferido de metadatos PE no prueba que el recurso haya sido modificado ni que pertenezca exclusivamente a la aplicación. No limpiar cambios ajenos o ambiguos.
- Si una implementación existente de LearningEngine contradice estas reglas, corregirla y volver a probarla antes de aprobar su fase.

### 3.3 Estrategias de portabilidad para ZIP

Aplicar este orden:

1.  Mecanismo nativo de la aplicación: argumentos, INI propio o modo
    portable soportado.
2.  Variables de entorno específicas del proceso mediante
    `CreateProcessW`, cuando la aplicación las respete.
3.  Renombrado de carpetas exclusivas existentes a `.pmbkp` y Directory
    Junction directa hacia los datos de la aplicación dentro de PM.

No introducir una cuarta estrategia sin aprobación expresa.

Antes de crear una Junction: - Identificar con suficiente certeza la
carpeta exclusiva de la aplicación. - Registrar si existía
originalmente. - No sobrescribir un `.pmbkp` existente. - Registrar la
intención en el manifiesto antes de modificar el anfitrión. - Verificar
que el punto de reanálisis y el destino corresponden a PM antes de
eliminarlo. - Eliminar únicamente la Junction propia; nunca seguirla
para borrar el destino. - Restaurar el `.pmbkp` si existía.

Si la atribución de una carpeta o clave es ambigua, no modificarla
automáticamente.

### 3.4 PortableApps / PAF

-   PortableApps y sus lanzadores gestionan sus propias aplicaciones.
-   PM no debe duplicar mecanismos que ya resuelve correctamente el
    lanzador PAF.
-   PM debe invocar el lanzador correspondiente y respetar sus
    parámetros y ciclo de vida.
-   Las aplicaciones PAF que limpian correctamente el anfitrión pueden
    clasificarse como verdes según la evidencia.
-   PM conserva la responsabilidad de su verificación general y de la
    respuesta crítica prioritaria.

### 3.5 PM y PMGuard (PMG)

-   PMG es una instancia de sombra del mismo ejecutable, invocada en
    modo `--guard` y ejecutada desde `%TEMP%`.
-   PMG utiliza el manifiesto y las recetas operativas que PM ya
    escribe; no mantiene una configuración paralela.
-   PMG solo ejecuta la limpieza de emergencia cuando PM ha desaparecido
    inesperadamente.
-   Si PM sigue activo, PMG no limpia.
-   Si PMG termina mientras PM sigue activo, PM continúa siendo
    responsable de la limpieza.
-   PM y PMG nunca pueden ejecutar simultáneamente las mismas
    operaciones de limpieza.
-   Utilizar exclusión mutua y comprobar el estado real de la sesión.
-   PMG debe esperar bloqueado por el proceso PM; no implementar polling
    continuo.
-   PMG no es un servicio de Windows ni un componente indispensable para
    el funcionamiento normal de PM.
-   La limpieza crítica tiene prioridad sobre el cierre cooperativo de
    PAF o de cualquier ZIP.

### 3.6 Cierres

Mantener flujos reutilizables y sencillos.

-   Cierre normal: cierre cooperativo cuando corresponda, seguido de la
    limpieza y restauración.
-   Cierre rápido: aplica el mecanismo aprobado para terminar los
    procesos controlados y ejecutar la misma restauración.
-   Cierre crítico: caída inesperada de PM, desconexión física de la
    fuente de PM o pérdida inesperada del origen. Debe cerrar los
    procesos controlados y ejecutar la limpieza/restauración crítica sin
    depender del USB.
-   No terminar procesos ajenos a PM.
-   No inventar una recuperación posterior de sesiones antiguas.
-   No hacer que un nuevo arranque de PM descubra y restaure
    automáticamente sesiones huérfanas.
-   Si PM y PMG desaparecen simultáneamente o se corta la electricidad,
    no prometer recuperación posterior garantizada.

El orden de limpieza debe respetar las dependencias: terminar primero
los procesos controlados; después retirar redirecciones y restaurar
carpetas y registro; finalmente retirar temporales propios cuando sea
seguro.

### 3.7 Manifiesto de sesión

-   Vive en `%TEMP%\PortableManager\Session\<SessionID>\session.ini`.
-   Es operativo y temporal; no es configuración persistente.
-   Debe registrar la intención antes de modificar el anfitrión
    (Write-Ahead).
-   Las operaciones inversas deben ser idempotentes.
-   Debe comprobar el estado real del recurso; no confiar únicamente en
    el último estado escrito.
-   Nunca sobrescribir ni eliminar respaldos que puedan ser necesarios
    para restaurar.
-   PMG puede abrir la sesión activa que PM le indicó.
-   No implementar recuperación automática de sesiones huérfanas en
    futuros arranques.

Estados de operación acordados:

- `INTENT_RECORDED`
- `APPLIED`
- `ROLLED_BACK`
- `FAILED`
- `SKIPPED`

`CLEANED` solo se declara cuando todas las operaciones necesarias han
terminado correctamente o en un estado terminal expresamente aceptado
por el diseño.

### 3.8 Respaldo del Registro

Se permite respaldar claves del registro mediante archivos `.reg` en el
directorio temporal de la sesión.

Flujo general: 1. Comprobar si la clave original existe. 2. Si existe,
exportarla a un `.reg` en `%TEMP%`. 3. Verificar que el respaldo se creó
y es válido antes de permitir la modificación. 4. Registrar ruta, clave
original y existencia previa en el manifiesto. 5. Al limpiar, eliminar
la clave modificada e importar el respaldo original. 6. Si originalmente
no existía, eliminar la clave creada por la aplicación. 7. Conservar el
`.reg` si la restauración falla.

No asumir que `RegCopyTreeW` equivale a exportar un `.reg`. Elegir y
documentar el mecanismo real de exportación/importación y probarlo en
claves sintéticas de HKCU.

No modificar HKLM ni claves reales de aplicaciones durante las pruebas.

### 3.9 Residuos y pmlog

-   Si la limpieza no puede completar una restauración, conservar los
    respaldos necesarios.
-   Crear `%TEMP%\pmlog` únicamente cuando queden operaciones pendientes
    o residuos que no se pudieron limpiar.
-   `pmlog` es un archivo de texto sin extensión.
-   Registrar solo rutas, operaciones pendientes y códigos de error.
-   No registrar credenciales, valores sensibles del registro ni
    contenido de archivos.
-   No crear el archivo si la limpieza termina correctamente.

## 4. Clasificación visual de aplicaciones

-   Blanco/gris: aplicación todavía no verificada.
-   Verde: no deja residuos en el anfitrión según las pruebas
    realizadas.
-   Amarillo: requiere la limpieza estándar de PM y, después de ella, el
    anfitrión queda restaurado.
-   Rojo: deja residuos que PM no puede identificar, limpiar o restaurar
    de forma segura.

Una aplicación no es roja simplemente por ser desconocida o por no haber
terminado su aprendizaje. El rojo requiere evidencia de residuos no
controlables. Una aplicación roja puede ejecutarse bajo responsabilidad
del usuario.

## 5. Alcance funcional conocido

PM debe evolucionar hacia un lanzador y gestor portable que permita:

-   Instalar aplicaciones portables.
-   Registrar y gestionar aplicaciones.
-   Lanzarlas mediante su ejecutable o lanzador correspondiente.
-   Cerrarlas con el sistema de limpieza de PM.
-   Restablecer los datos de una aplicación.
-   Archivar una aplicación conservando sus datos.
-   Desinstalarla y ejecutar la limpieza correspondiente.
-   Gestionar una carpeta virtual de documentos.

No adelantar funciones de interfaz ni de gestión que no pertenezcan a la
fase actualmente autorizada.

Las ideas futuras no son alcance aprobado hasta que el propietario las
priorice expresamente.

## 6. Tecnología y compilación

Toolchain oficial de desarrollo:

- LLVM-MinGW portable.
- Clang.
- C++20.
- Windows x64.
- UCRT.

Directivas:

- Utilizar Win32 API para las funciones nativas de Windows.
- No utilizar C# para el ejecutable final.
- No incorporar Python al runtime final. Los scripts Python existentes pueden mantenerse como herramientas auxiliares de auditoría.
- Mantener el toolchain dentro de `tools/toolchains/`; no incluirlo en la distribución final de PM.
- Mantener compilación reproducible y sin dependencias innecesarias.
- Conservar las opciones de compilación aprobadas en la Fase 0B (`-static`, `-municode` y `/SWAPRUN:CD` mediante LLD, además de las advertencias configuradas).
- No afirmar que LLVM-MinGW `-static` es idéntico a MSVC `/MT`.
- Verificar arquitectura PE, imports y características del ejecutable; no confiar solo en que el linker acepte una opción.

No cambiar el toolchain ni las opciones base sin justificarlo y pedir
aprobación.

### Sesiones Codex Cloud (Linux)

Al iniciar cada sesión, activar las herramientas existentes solo para el proceso:

```bash
export PATH="/workspace/.pm-cloud-tools/bin:/workspace/PortableManager-Lite/tools/toolchains/llvm-mingw-linux/bin:$PATH"
```

Ejecutables disponibles: `cmake`, `ninja`, `clang++`, `llvm-readobj` y
`x86_64-w64-mingw32-clang++`. `clang++` sin target explícito es un compilador
host Linux; para compilar directamente Windows x64 usar
`x86_64-w64-mingw32-clang++` o `clang++ --target=x86_64-w64-windows-gnu`.
El toolchain CMake del proyecto fija ese target. Comprobar con `command -v`
y `--version`; no reinstalar herramientas que ya funcionan. Los ejecutables
PE se prueban en GitHub Actions Windows, nunca se consideran ejecutados por
el mero hecho de compilar en Linux.

## 7. Seguridad de pruebas

Por defecto, todas las pruebas deben ser seguras, aisladas y
reversibles.

-   Usar carpetas sintéticas y GUID dentro de `%TEMP%`.
-   Usar claves exclusivas de prueba bajo HKCU.
-   Usar ejecutables auxiliares creados para las pruebas.
-   No modificar datos personales.
-   No usar Firefox, PuTTY u otras aplicaciones reales en pruebas
    unitarias.
-   No modificar HKLM.
-   No terminar procesos ajenos.
-   No eliminar archivos temporales que no pertenezcan a la prueba.
-   No conectar, desconectar ni modificar físicamente un USB sin
    autorización explícita del propietario.
-   No ejecutar pruebas destructivas sobre el anfitrión sin explicar
    antes su alcance y obtener autorización.
-   No afirmar que una simulación equivale a una prueba física real.
-   Separar evidencia histórica de la nueva evidencia de implementación.

Estados de prueba permitidos:

- `passed`
- `failed`
- `blocked`
- `not_run`

No cambiar los criterios de aceptación para conseguir un resultado
positivo. No marcar una fase como aprobada solo porque compile.

## 8. Organización del repositorio y documentación por fase

La estructura documental obligatoria utiliza carpetas con dos dígitos:

-   `docs/fase01/`
-   `docs/fase02/`
-   `docs/fase03/`
-   `docs/fase04/`
-   `docs/fase05/`
-   `docs/fase06/`
-   `docs/fase07/`
-   `docs/fase08/`

Reglas:

- No crear carpetas paralelas como `docs/fase2/` cuando corresponde `docs/fase02/`.
- Antes de mover o renombrar una carpeta existente, inspeccionar su contenido y preservar todos los archivos.
- El código fuente permanece en `src/`; no duplicar código dentro de las carpetas de fase.
- Cada carpeta de fase conserva su README, alcance, decisiones específicas, evidencias históricas y ejemplos. Los scripts de prueba generan sus informes en `reports/` o en la ruta de salida que indique su ejecución.
- Los informes JSON históricos deben preservarse intactos. Las nuevas ejecuciones generan archivos nuevos.
- Los documentos generales de `docs/` describen el estado vigente del proyecto.
- Al cerrar una fase, actualizar los documentos generales y consolidar los resultados de esa fase en su carpeta.
- No mantener copias divergentes del mismo documento sin indicar cuál es la fuente vigente.
- Registrar claramente qué está aprobado, pendiente, bloqueado o no probado.

Estructura técnica habitual: - `src/` - `tests/` - `reports/` -
`tools/` - `docs/PMDocs/` - `docs/fase01/` ... `docs/fase08/`

Respetar la estructura real del repositorio; no reorganizarlo
masivamente sin autorización.

## 9. Flujo de trabajo por fases

El proyecto se desarrolla secuencialmente y con puertas de aprobación
humanas.

Para cada fase:

1.  Leer este archivo y toda la documentación vigente relevante.
2.  Inspeccionar el código existente antes de crear o reemplazar
    archivos.
3.  Confirmar alcance, dependencias, criterios de aceptación y
    exclusiones.
4.  Implementar solo la fase autorizada.
5.  Compilar.
6.  Ejecutar pruebas unitarias y de integración pertinentes.
7.  Ejecutar regresiones de las fases anteriores que correspondan.
8.  Revisar errores, advertencias y seguridad.
9.  Generar un JSON de resultados con evidencia verificable.
10. Actualizar la documentación general.
11. Consolidar documentación y evidencias en `docs/faseNN/`.
12. Presentar resumen de archivos cambiados, pruebas ejecutadas,
    resultados, riesgos y pendientes.

**No iniciar la fase siguiente automáticamente.** Detenerse y esperar
aprobación expresa del propietario.

Si una prueba falla: - Detener el avance. - Identificar causa. -
Corregir dentro del alcance autorizado. - Repetir las pruebas
afectadas. - Documentar el resultado real.

No repetir pruebas históricas ya aprobadas si no existe una razón
técnica. No sobrescribir sus informes.

## 10. Git, ramas y Codex Cloud

-   El repositorio de GitHub debe ser privado salvo decisión expresa del
    propietario.
-   No incluir secretos, tokens, credenciales, archivos personales,
    volcados de registro reales ni datos sensibles.
-   Revisar `.gitignore` antes del primer push.
-   No subir carpetas de build, temporales, caches, ejecutables de
    prueba generados ni toolchains descargados si no son necesarios para
    reproducir el entorno.
-   Sí conservar los fuentes, CMake/proyectos, scripts reproducibles,
    documentación, informes JSON y evidencias que formen parte del
    proyecto.
-   Trabajar en una rama de tarea/fase; no modificar directamente `main`
    desde tareas autónomas.
-   Presentar cambios mediante Pull Request o revisión de diff.
-   No fusionar a `main`, borrar ramas ni reescribir historial sin
    autorización expresa.
-   No hacer push de secretos ni añadir credenciales al entorno.
-   Las tareas de Codex Cloud son espacios de trabajo separados. Guardar
    el trabajo importante en commits; no depender de archivos sin
    confirmar o de la persistencia temporal de una VM.
-   Si el entorno cloud no puede ejecutar una prueba específica de
    Windows, marcarla `blocked` o `not_run`, explicar el motivo y
    proponer una prueba adecuada. No simular un `passed`.

## 11. Límites de autonomía

El agente puede inspeccionar, planificar, programar, compilar, ejecutar
pruebas seguras y documentar dentro de la fase autorizada.

Debe pedir autorización antes de: - Iniciar una fase posterior. -
Cambiar una decisión arquitectónica aprobada. - Añadir una dependencia
importante. - Introducir servicios, drivers, virtualización o
componentes residentes. - Realizar pruebas físicas de desconexión USB. -
Modificar datos reales del anfitrión. - Ejecutar limpiezas destructivas
fuera de directorios sintéticos. - Cambiar el toolchain. - Fusionar Pull
Requests o publicar releases. - Eliminar evidencias, respaldos o
archivos preexistentes.

Cuando falte información, no inventar. Presentar la duda más importante
y detener únicamente el trabajo que dependa de ella.

## 12. Formato de entrega al finalizar una tarea

Toda entrega debe incluir:

1.  Fase y alcance trabajado.
2.  Resumen de implementación.
3.  Archivos creados y modificados.
4.  Comandos de compilación ejecutados.
5.  Pruebas ejecutadas y sus resultados.
6.  Ruta del JSON de evidencia.
7.  Estado de regresión.
8.  Riesgos, limitaciones y pruebas pendientes.
9.  Documentación consolidada en `docs/faseNN/`.
10. Confirmación explícita de que no se inició la siguiente fase.

No usar expresiones como "todo funciona" o "fase aprobada" sin evidencia
que respalde la afirmación.
