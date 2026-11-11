# PortableManager Lite --- Arquitectura técnica de PM Core y PMGuard

**Versión:** 1.1\
**Fecha:** 11 de noviembre de 2026\
**Estado:** diseño técnico consolidado para validación; no es una
implementación.\
**Documentos funcionales:** `PortableManager-Lite-Diseno-v0.1-base.md` y
`PortableManager-Lite-Diseno-v0.2.1.md`.\
**Regla de precedencia:** v0.2.1 prevalece sobre v0.1. Este documento
desarrolla técnicamente esas decisiones y no las reemplaza.

------------------------------------------------------------------------

## 1. Principios rectores

PortableManager Lite (PM) es un lanzador y entorno portátil para
Windows. Administra aplicaciones y documentos del USB y reduce los
rastros de información personal en el anfitrión mediante recetas
concretas, respaldos previos y restauración.

Principios:

1.  Priorizar mecanismos nativos de cada aplicación para que sus datos
    permanezcan directamente en el USB.
2.  No modificar variables globales de Windows ni requerir instalación,
    drivers, servicios o privilegios de administrador para el flujo
    ordinario.
3.  Modificar solo rutas y claves identificadas con suficiente certeza.
4.  Preservar primero el estado preexistente del anfitrión; no borrar
    datos ambiguos o compartidos.
5.  Restaurar los cambios del anfitrión que PM haya preparado y
    registrado.
6.  Mantener la configuración permanente y los datos portables en el
    USB.
7.  Mantener en TEMP únicamente el runtime y el manifiesto transitorio
    imprescindibles para cerrar/restaurar si el USB desaparece.
8.  No prometer cero rastros ni virtualización universal de Windows.

PM no es un sandbox ni un virtualizador universal.

## 2. Configuración y almacenamiento

La estructura conceptual vigente es:

``` text
<USB>\
├── pm.exe
├── App\
│   ├── .temp\
│   ├── FirefoxPortable\
│   ├── NotepadPlusPlusPortable\
│   └── ...
├── Data\
│   ├── apps.ini
│   ├── config.ini
│   ├── recipes\
│   ├── apps\
│   │   └── <id-estable>\
│   │       ├── roaming\
│   │       ├── local\
│   │       └── registry\
│   ├── archived\
│   ├── logs\
│   └── session.journal
└── Other\
```

Responsabilidades:

-   `Data/config.ini`: configuración general de PM.
-   `Data/apps.ini`: identidad estable de aplicaciones, nombres, alias,
    lanzadores, asociaciones y estado.
-   `Data/recipes/`: recetas permanentes por aplicación, incluidas rutas
    aprendidas, claves y exclusiones.
-   `Data/apps/<id-estable>/`: datos portables persistentes de
    aplicaciones ZIP.
-   `Data/archived/`: datos de aplicaciones PAF archivadas, según la
    política de v0.2.
-   `Data/logs/`: incidencias o residuos que se decida conservar,
    exclusivamente en el USB y cuando esté disponible.
-   `Data/session.journal`: diario de operaciones de
    instalación/actualización en el USB. No es el mecanismo de
    restauración del anfitrión.
-   `App/`: ejecutables y paquetes.
-   `Other/`: documentos y carpetas del usuario.

No se utiliza un `pm.ini` obligatorio en la raíz. Los nombres exactos de
archivos y el esquema INI siguen siendo conceptuales hasta su
implementación.

### Dos tipos de receta/estado que no deben confundirse

**Receta permanente:** vive en `Data/recipes/` del USB. Describe cómo
tratar una aplicación en ejecuciones futuras.

**Manifiesto operativo de sesión:** vive temporalmente en
`%TEMP%\\PortableManager\\Session\\<SessionID>\\session.ini`. PM lo
genera a partir de las recetas permanentes y registra las operaciones
concretas de la sesión: rutas, claves, respaldos, ausencias originales,
estado de cada paso y operaciones inversas pendientes. PM lo lee durante
su cierre; PMGuard (PMG) lee exactamente el mismo archivo si PM
desaparece.

El manifiesto TEMP no es historial, no contiene los perfiles portables
completos y se elimina después de una restauración verificada.

## 3. Orden de portabilización para aplicaciones ZIP

El orden es obligatorio. La tercera opción es un caso borde, no el
comportamiento predeterminado.

### Prioridad 1 --- Mecanismos nativos de la aplicación

Utilizar argumentos, archivos INI/XML/JSON, opciones documentadas u
otros mecanismos propios que permitan dirigir los datos de usuario a
`Data/apps/<id-estable>/` en el USB.

Ejemplos de argumentos como `--user-data-dir` o `--config-dir` son
ilustrativos: solo se usarán cuando estén documentados o hayan sido
probados para esa aplicación.

### Prioridad 2 --- Variables de entorno y adaptadores de PM

PM puede construir un bloque de entorno específico para el proceso
mediante `CreateProcess`. Los procesos hijos heredan ese entorno. PM
puede configurar `APPDATA`, `LOCALAPPDATA`, `TEMP`, `TMP` u otras
variables solo cuando se haya comprobado que la aplicación las respeta.

Esto no modifica las variables globales de Windows ni obliga a una
aplicación a utilizar esas variables en lugar de las API de carpetas
conocidas o el registro.

### Prioridad 3 --- Preparación temporal de rutas del anfitrión con `.pmbkp`

Solo si las dos prioridades anteriores no son viables:

1.  Identificar una ruta exclusiva de la aplicación.
2.  Si existe, renombrar la carpeta en el mismo volumen: `Carpeta` →
    `Carpeta.pmbkp`.
3.  Si no existía, registrar expresamente que la ruta original estaba
    ausente.
4.  Crear la ruta de trabajo y colocar allí los datos portables de la
    aplicación desde el USB.
5.  Ejecutar la aplicación.
6.  En cierre normal, guardar los datos nuevos/modificados en
    `Data/apps/<id-estable>/`.
7.  Eliminar la ruta de trabajo y restaurar `.pmbkp`; si no existía ruta
    original, dejarla ausente.

El renombrado conserva el estado original sin copiarlo. Si ya existe un
`.pmbkp`, PM no lo sobrescribe y no inicia esa operación.

### Carpetas compartidas

-   Una carpeta de fabricante puede contener varias aplicaciones y datos
    de usuario.
-   Si el padre es compartido, solo se puede trabajar sobre una
    subcarpeta exclusiva y verificada.
-   No se renombra una carpeta compartida completa.
-   Si no hay una frontera exclusiva segura, PM no modifica esa
    ubicación y no promete limpiarla.
-   Si una carpeta exclusiva contiene archivos mezclados que PM no puede
    clasificar, no se eliminan por inferencia.
-   No seguir junctions, enlaces simbólicos ni puntos de reanálisis
    durante borrados recursivos sin una validación explícita.
-   Las carpetas de instalación dentro de `App/` no son objetivos de
    limpieza del anfitrión.

## 4. Aplicaciones PortableApps (PAF)

Los lanzadores oficiales PortableApps administran su propio perfil,
estructura `Data`, movimientos y limpieza. PM no duplica ni superpone
una segunda restauración destructiva sobre esas recetas.

-   En cierre normal, PM permite que el lanzador PAF ejecute su rutina y
    espera sus procesos.
-   PM puede observar el resultado y tratar residuos adicionales solo
    con recetas exactas y verificadas.
-   PM no mueve el perfil portable PAF ni el perfil de una instalación
    ajena de la misma aplicación.
-   Para FirefoxPortable, las pruebas del proyecto observaron el perfil
    portable y preferencias persistentes en el USB; esto no demuestra
    ausencia absoluta de rastros auxiliares en Windows.

**Cierre crítico:** prevalece sobre la rutina PAF. PM no espera al
lanzador; termina los procesos bajo su responsabilidad y restaura lo que
PM tenga registrado en el manifiesto. Si la rutina PAF se interrumpe, PM
no afirma que PAF haya completado su propia limpieza. Los residuos que
no estén bajo control de PM pueden permanecer y deben reflejarse en el
estado de privacidad cuando puedan verificarse.

## 5. Aprendizaje inicial de aplicaciones ZIP

El aprendizaje es observación controlada y comparación, no IA ni
virtualización universal.

### Identificación y candidatos

PM utiliza:

-   Nombre de la aplicación y del ejecutable.
-   Metadatos PE disponibles: `CompanyName`, `ProductName`,
    `FileDescription`.
-   Fabricante/publisher.
-   Nombre de carpeta y paquete.
-   Alias definidos por el usuario.

Con estos datos registra candidatos de rutas y claves, por ejemplo
carpetas específicas bajo `%APPDATA%` y `%LOCALAPPDATA%`,
`%TEMP%`/`%TMP%` y claves `HKCU\\Software\\<Publisher>` o
`HKCU\\Software\\<Application>`.

La coincidencia de nombres es una pista, no prueba suficiente para
borrar.

### Primera ejecución

1.  PM calcula rutas y claves candidatas.
2.  Captura el baseline previo de los candidatos y registra qué
    rutas/claves existían.
3.  Respalda mediante `.pmbkp` solo las carpetas exclusivas que vaya a
    sustituir.
4.  Ejecuta la aplicación en modo de aprendizaje sin cerrarla
    automáticamente.
5.  Observa cambios en las ubicaciones acotadas.
6.  Al terminar, compara el estado posterior con el baseline.
7.  Clasifica los cambios por archivos, directorios y registro.
8.  Guarda en el USB los datos nuevos atribuibles a la aplicación.
9.  Limpia y restaura únicamente los cambios que pueda atribuir y
    revertir con suficiente certeza.
10. Guarda o amplía la receta permanente en `Data/recipes/`.

La barra blanca de la píldora indica primera ejecución/aprendizaje. No
se inventan porcentajes mientras la aplicación está abierta si no hay
una unidad de trabajo medible.

### Límites

-   Las notificaciones de cambios no identifican por sí solas el proceso
    autor.
-   Una comparación posterior no reconstruye el contenido anterior si no
    hubo baseline o respaldo.
-   No se borra un cambio ambiguo ni se renombra una carpeta compartida.
-   La primera ejecución no garantiza descubrir todas las escrituras.
-   Una aplicación sin receta segura permanece blanca/gris (sin
    verificar).

## 6. Registro de Windows

### Identificación de claves candidatas

La primera aproximación es registrar previamente claves HKCU candidatas
por nombre de aplicación, ejecutable, fabricante/publisher y alias.
Ejemplos:

``` text
HKCU\\Software\\<Publisher>\\<Application>
HKCU\\Software\\<Application>
```

Si una clave no existe, PM puede observar el padre candidato y comparar
nombres/valores antes y después. No se realiza un snapshot
indiscriminado de todo HKCU.

### Captura y restauración

Para cada clave exacta aprobada por la receta:

1.  Capturar recursivamente el estado original: existencia, subclaves,
    nombres de valores, tipos y datos.
2.  Guardar el baseline en el área local temporal de la sesión,
    referenciado desde el manifiesto TEMP.
3.  Cargar los datos portables de esa aplicación desde
    `Data/apps/<id-estable>/registry/`, cuando corresponda.
4.  Ejecutar la aplicación.
5.  En cierre normal, guardar el estado portable actualizado en el USB.
6.  Retirar el estado temporal de sesión y restaurar el baseline
    original del anfitrión.
7.  Verificar el resultado.

En cierre crítico no se intenta persistir el estado actual de la
aplicación al USB: se terminan los procesos y se restaura el baseline
del anfitrión.

### Observación y límites

`RegNotifyChangeKeyValue` notifica cambios en una clave y, si se
solicita, sus subclaves. No identifica qué proceso hizo el cambio ni
entrega el snapshot anterior. PM combina notificaciones con baseline y
comparación posterior.

Alcance inicial: HKCU y rutas explícitas. No tocar HKLM ni raíces
globales. Si la clave es compartida, hay cambios concurrentes no
atribuibles o no puede restaurarse con seguridad, PM no la modifica a
ciegas y la aplicación no obtiene estado verde.

## 7. PMGuard (PMG)

PMG es una sombra de emergencia de PM, no un segundo gestor ni un
coordinador de configuración.

### Modelo de responsabilidad

-   PM es el proceso principal y propietario de la sesión.
-   PM prepara el runtime local y escribe el manifiesto operativo en
    TEMP.
-   PM lee el manifiesto durante su cierre normal, rápido o crítico.
-   Si PM desaparece inesperadamente, PMG lee el mismo manifiesto y
    ejecuta la rutina de emergencia.
-   PMG no recibe recetas mediante una tubería de mensajes ni mantiene
    una copia de `Data/recipes/`.
-   PMG no es necesario para el funcionamiento normal de PM.

**Si PMG desaparece mientras PM sigue vivo, no se inicia un cierre de
emergencia por ese solo hecho.** PM continúa siendo responsable de la
sesión; puede reiniciar PMG si lo considera necesario o continuar sin él
y ejecutar la limpieza cuando corresponda. La pérdida de PMG no obliga a
cerrar PM.

### Manifiesto temporal

Ruta conceptual:

``` text
%TEMP%\\PortableManager\\Session\\<SessionID>\\session.ini
```

Contiene únicamente datos necesarios para revertir la sesión activa:

-   Identificador de sesión y aplicaciones activas.
-   Rutas originales y rutas de trabajo.
-   Si cada ruta existía antes.
-   Rutas `.pmbkp`.
-   Claves exactas del registro y referencias a sus snapshots
    originales.
-   Estado de cada operación y acción inversa pendiente.
-   Nombres de los Job Objects asociados.
-   Información mínima para identificar procesos y transacciones.

No contiene historiales ni informes. Los respaldos de carpetas
permanecen renombrados en su ubicación original; los snapshots pequeños
del registro pueden guardarse junto al manifiesto en TEMP.

PM escribe el manifiesto antes de cada operación destructiva y actualiza
el estado después de completarla. Las actualizaciones deben realizarse
mediante escritura temporal y sustitución controlada, de modo que PMG no
lea un archivo parcialmente escrito.

### Exclusión mutua

Si PM y PMG detectan simultáneamente una emergencia, solo uno puede
restaurar. Un mutex local por sesión o mecanismo equivalente concede la
ejecución al primer proceso que lo adquiere. El otro comprueba el estado
y no repite las operaciones.

### Runtime local y desconexión USB

El diseño prevé preparar copias locales del runtime de PM y PMG en
`%TEMP%` antes de iniciar aplicaciones. Esto permitiría ejecutar el
cierre si desaparece el USB. Es una hipótesis de diseño pendiente de
prueba: deben comprobarse dependencias, carga de módulos, permisos,
antivirus y comportamiento real de Windows. La memoria del proceso no
garantiza que todos los componentes estén residentes ni excluidos del
archivo de paginación.

Si PM y PMG desaparecen simultáneamente, Windows se apaga abruptamente o
el manifiesto/los respaldos locales dejan de estar accesibles, no se
garantiza restauración completa.

## 8. Rutinas de cierre

### Cierre normal

1.  Bloquear nuevos lanzamientos.
2.  Solicitar cierre normal a las aplicaciones.
3.  Permitir que los lanzadores PAF ejecuten su propia rutina.
4.  Esperar la terminación de los procesos de cada aplicación.
5.  Guardar en el USB los datos portables ZIP que correspondan.
6.  Ejecutar las recetas PM.
7.  Eliminar las rutas de trabajo temporales.
8.  Restaurar `.pmbkp` y los snapshots originales del registro.
9.  Verificar lo posible.
10. Eliminar el manifiesto TEMP tras una restauración verificada y
    cerrar PM/PMG.

### Cierre rápido

Reduce el tiempo de espera. No omite la restauración esencial del
anfitrión. Si un proceso no termina dentro del plazo, PM puede forzarlo
y continuar con la restauración; en ese caso, los últimos datos de la
aplicación pueden no haberse guardado correctamente en el USB.

### Cierre crítico y emergencia

El cierre crítico solicitado por el usuario, la desconexión USB o la
caída de PM activan la prioridad de restauración:

1.  Bloquear nuevos lanzamientos.
2.  Terminar inmediatamente los Job Objects y los procesos que PM pueda
    controlar.
3.  No esperar la rutina normal de PAF ni de aplicaciones ZIP.
4.  No intentar guardar en el USB los datos de aplicación todavía no
    persistidos.
5.  Eliminar las rutas de trabajo creadas por PM.
6.  Restaurar las carpetas `.pmbkp` y las claves originales registradas.
7.  Verificar lo posible con los recursos locales.
8.  Finalizar PM/PMG.

Si PM cae, PMG ejecuta esta misma rutina con el manifiesto. Si PMG cae,
PM continúa como responsable. La pérdida de PMG no es por sí misma un
disparador de emergencia.

No se escriben informes persistentes en el anfitrión. Los logs
opcionales de residuos/incidencias se guardan solo en el USB cuando esté
disponible.

## 9. Job Objects

Se propone utilizar Job Objects nativos de Windows para agrupar procesos
por instancia de aplicación.

-   Un Job Object nombrado por instancia.
-   PM crea y mantiene un handle; PMG puede abrir el mismo objeto
    nombrado con permisos adecuados.
-   Los procesos hijos creados normalmente heredan la pertenencia al
    Job, salvo mecanismos de breakaway o procesos creados por vías que
    no hereden esa relación.
-   PM y PMG pueden terminar el mismo grupo mediante el objeto
    compartido.
-   `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` termina los procesos cuando se
    cierra el **último handle** del Job Object, no cuando lo cierra
    cualquiera de los dos procesos.

La asignación de procesos, apertura compartida, seguridad del objeto y
posibles procesos que escapen deben probarse. Job Objects no garantizan
por sí solos capturar absolutamente todos los procesos auxiliares de
cualquier aplicación.

## 10. Estados visuales de privacidad

Los estados de privacidad del icono de aplicación son independientes del
LED global de PM y del icono contextual de instalación.

  ---------------------------------------------------------------------
  Estado                             Significado
  ---------------------------------- ----------------------------------
  Blanco/gris                        Sin verificar: aplicación nueva o
                                     receta no verificada

  Verde                              Al cerrarse, no deja residuos
                                     observados que requieran limpieza
                                     adicional de PM

  Amarillo                           Necesita limpieza estándar de PM
                                     y, después de ella, el anfitrión
                                     queda restaurado respecto de los
                                     cambios controlados

  Rojo                               Persisten residuos observados o
                                     hay conflictos/restauraciones
                                     insuficientes
  ---------------------------------------------------------------------

Los colores expresan el resultado dentro del alcance comprobado; no
garantizan ausencia absoluta de rastros en Windows.

## 11. Referencias técnicas y proyectos estudiados

### PortableApps.com Launcher

Se toma como referencia para el manejo de archivos, directorios y
registro:

-   `[FilesMove]` y `[DirectoriesMove]` respaldan destinos preexistentes
    y los restauran al finalizar.
-   `[RegistryKeys]` permite conservar datos de registro portables y
    restaurar la clave preexistente del anfitrión.
-   `[DirectoriesCleanupIfEmpty]` evita dejar padres vacíos.

Documentación: -
https://portableapps.com/manuals/PortableApps.comLauncher/ref/launcher.ini/filesystem.html -
https://portableapps.com/manuals/PortableApps.comLauncher/topics/registry.html -
https://portableapps.com/manuals/PortableApps.comLauncher/ref/index.html

### API de Windows

-   Variables de entorno y bloque específico para `CreateProcess`:
    https://learn.microsoft.com/en-us/windows/win32/procthread/environment-variables
-   `RegNotifyChangeKeyValue`:
    https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regnotifychangekeyvalue
-   `ReadDirectoryChangesW`:
    https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw
-   Job Objects:
    https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects

Limitaciones técnicas relevantes:

-   `RegNotifyChangeKeyValue` notifica cambios, no identifica el proceso
    autor ni conserva el estado previo.
-   `ReadDirectoryChangesW` puede descartar el búfer completo si se
    desborda; ante esa condición se debe volver a enumerar y comparar la
    ruta observada.
-   `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` actúa al cerrarse el último
    handle. Los procesos que escapen del Job no quedan cubiertos por esa
    protección.

### Referencias conceptuales

Cameyo, ThinApp, Turbo Studio y Sandboxie-Plus se estudiaron como
referencias de aislamiento/virtualización. PM Lite no implementa sus
motores, drivers ni interceptación universal.

## 12. Fuera del alcance de Lite

-   Virtualización universal del filesystem o del registro.
-   Drivers, servicios persistentes y hooks globales.
-   Machine learning para crear recetas automáticamente.
-   Limpieza indiscriminada de rastros de Windows.
-   Modificación de HKLM o claves globales.
-   Garantía de cero residuos en todos los equipos.
-   Recuperación de datos de aplicación no guardados durante un cierre
    crítico.
-   Garantía frente a corte eléctrico, apagado abrupto o desaparición
    simultánea de PM y PMG.
-   Cifrado Secure y sincronización Cloud/rclone.
-   Ejecución universal desde WebDAV o unidades virtuales.

## 13. Criterios de aceptación antes de implementar la interfaz

1.  La receta nativa puede dirigir datos al USB sin modificar el
    anfitrión.
2.  El bloque de entorno de PM afecta solo al proceso y sus
    descendientes.
3.  La tercera prioridad utiliza `.pmbkp` solo para carpetas exclusivas
    y registra correctamente la ausencia original.
4.  Las carpetas compartidas no se mueven completas.
5.  Los datos portables ZIP se cargan desde el USB, se guardan en cierre
    normal y no se confunden con el respaldo del anfitrión.
6.  El aprendizaje compara baseline y estado posterior sin borrar
    cambios ambiguos.
7.  La transacción de registro restaura valores, tipos, datos y
    subclaves originales dentro de su alcance.
8.  PM escribe y lee el manifiesto TEMP; PMG puede leer el mismo
    manifiesto si PM desaparece.
9.  La pérdida de PMG no cierra la sesión ni dispara por sí sola una
    emergencia.
10. Si PM desaparece, PMG ejecuta la rutina de emergencia.
11. PM y PMG no ejecutan simultáneamente la misma restauración.
12. El cierre crítico no espera a PAF ni a aplicaciones ZIP y prioriza
    restaurar el anfitrión.
13. La rutina de emergencia no depende del USB, sujeto a validar el
    runtime local.
14. Los Job Objects compartidos funcionan y se conocen sus límites de
    captura.
15. No se generan historiales ni informes persistentes en el anfitrión.

**Principio final:** PM conserva en el USB la configuración, las recetas
permanentes y los datos portables. En el anfitrión utiliza únicamente
rutas de trabajo controladas y un manifiesto temporal que permite
deshacer los cambios que PM mismo preparó.
