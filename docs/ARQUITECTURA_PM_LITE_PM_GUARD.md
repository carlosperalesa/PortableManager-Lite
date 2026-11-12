# PortableManager Lite --- Cierre de diseño de PM Core y PMGuard

**Estado:** propuesta consolidada para aprobación\
**Fecha:** 11 de noviembre de 2026\
**Alcance:** motor de portabilidad, aprendizaje inicial, transacciones
del anfitrión y PMGuard.\
**No incluye:** interfaz gráfica definitiva, bóveda Secure,
sincronización Cloud ni virtualización universal.

> **Nota de vigencia.** Esta versión conserva el diseño histórico de PM Lite.
> Para el contrato vigente de PML, `pml.exe --guard`, PMD y `wrapper.ini` por
> instalación, consultar [arquitectura nativa](ARQUITECTURA_NATIVE.md),
> [Fase 6](fase06/README_FASE06.md) y [Fase 7](fase07/README_FASE07.md).

## 1. Principios rectores

PortableManager Lite es un lanzador de aplicaciones portables con un
motor pequeño de recetas. Su prioridad es:

1.  Ejecutar aplicaciones conservando sus datos en el USB cuando la
    aplicación lo permita.
2.  Evitar que las aplicaciones dejen datos en el anfitrión.
3.  Eliminar los cambios temporales del anfitrión y restaurar el estado
    previo.
4.  Mantener el motor ligero, sin drivers, servicios ni privilegios de
    administrador.
5.  No responsabilizarse de la integridad interna de una aplicación ni
    de sus datos si el USB se desconecta durante una escritura.

PM no es un virtualizador universal de Windows.

## 2. Configuración y almacenamiento

La configuración permanente de PM permanece en el USB y se lee
directamente desde allí.

``` text
USB:\
├── pm.exe
├── pm.ini
├── App\
│   ├── FirefoxPortable\
│   ├── PuTTY\
│   └── ...
├── Data\
└── Other\
```

Reglas:

-   PM no copia `pm.ini` completo al anfitrión.
-   PM no mantiene historiales de sesión ni informes de cierre.
-   PM escribe en `pm.ini` cuando ocurre un cambio real: instalación,
    desinstalación, configuración, asociación o aprendizaje de una
    receta.
-   La configuración de aplicaciones y sus datos portables permanecen en
    el USB.
-   El runtime ejecutable de PM y PMGuard se prepara localmente en
    `%TEMP%` para que la retirada del USB no elimine el código que debe
    realizar la limpieza.

La copia local del ejecutable no debe confundirse con una copia de la
configuración o de los datos del usuario.

## 3. Orden obligatorio de portabilización

Para aplicaciones ZIP, PM utiliza este orden:

### Prioridad 1 --- Mecanismos nativos de la aplicación

Usar argumentos, archivos de configuración o parámetros documentados que
permitan establecer una ruta de datos dentro del USB.

Ejemplos conceptuales: `--user-data-dir`, `--config-dir` o una opción
propia de la aplicación. Nunca inventar argumentos: deben estar
documentados o probados.

### Prioridad 2 --- Variables de entorno y adaptadores del lanzador PM

PM puede crear un bloque de entorno específico para el proceso y sus
descendientes, sin modificar las variables globales de Windows.

Puede configurar `APPDATA`, `LOCALAPPDATA`, `TEMP` y `TMP` u otras
variables solo cuando la aplicación realmente las respete. No todas las
aplicaciones usan estas variables para resolver sus carpetas conocidas;
algunas consultan directamente las API de Windows o el registro.

Los valores se definen por aplicación en `pm.ini`. PM entrega a
`CreateProcess` un bloque de entorno propio para esa aplicación; no
cambia las variables globales de Windows. Los procesos hijos heredan ese
entorno. Esto solo funciona si la aplicación consulta esas variables: no
obliga a una aplicación a dejar de usar las API de carpetas conocidas de
Windows.

### Prioridad 3 --- Movimiento y restauración de rutas concretas

Solo cuando las dos opciones anteriores no sirven:

1.  Identificar una carpeta exclusiva de la aplicación.
2.  Renombrarla en el mismo volumen, por ejemplo `Carpeta` →
    `Carpeta.pmbkp`.
3.  Crear la ruta de trabajo vacía.
4.  Ejecutar la aplicación.
5.  Al cerrar, eliminar la ruta de trabajo completa.
6.  Restaurar `Carpeta.pmbkp` a su nombre original.

El renombrado evita copiar innecesariamente los datos originales y
conserva el respaldo en el mismo volumen.

### Regla sobre carpetas compartidas

Una carpeta de fabricante no necesariamente contiene solo archivos del
programa. Puede contener preferencias, perfiles o datos de usuario de
varias aplicaciones.

Ejemplos de padres potencialmente compartidos:

-   `%APPDATA%\Mozilla`
-   `%APPDATA%\Microsoft`
-   `%LOCALAPPDATA%\Google`

Por tanto:

-   Si existe una carpeta exclusiva de la aplicación, se puede renombrar
    completa.
-   Si el padre es compartido, se trabaja sobre la subcarpeta exclusiva.
-   Si no existe un límite exclusivo y seguro, PM no mueve el padre
    completo.
-   Si tampoco puede aislar archivos concretos de forma fiable, la
    aplicación queda sin verificar y no se promete limpieza completa.

Las carpetas de instalación de la aplicación dentro de `App` no son
objetivos de esta limpieza.

## 4. Aplicaciones PAF y FirefoxPortable

PortableApps administra sus propios lanzadores, perfiles y estructura
`Data`. PM no debe duplicar esa función.

Para FirefoxPortable, el INI del lanzador define rutas portables como:

``` ini
[FirefoxPortable]
FirefoxDirectory=App\Firefox
ProfileDirectory=Data\profile
SettingsDirectory=Data\settings
PluginsDirectory=Data\plugins
FirefoxExecutable=firefox.exe
```

Por tanto:

-   El perfil de FirefoxPortable permanece en el USB.
-   PM no mueve ni respalda el perfil portable.
-   PM no toca el perfil del Firefox instalado en Windows.
-   PortableApps ejecuta su propio cierre y limpieza durante el cierre
    normal.
-   PM puede limpiar residuos adicionales solo mediante recetas exactas
    y verificadas.

Las pruebas anteriores observaron el perfil y las preferencias
persistentes en el USB, además de residuos auxiliares relacionados con
Mozilla en el anfitrión. Eso no demuestra que el perfil se haya guardado
localmente.

En cierre crítico, desconexión USB o caída de PM, la prioridad es
terminar los procesos y restaurar el anfitrión; no se espera a que el
lanzador PAF complete su rutina normal.

## 5. Aprendizaje inicial de aplicaciones ZIP

El aprendizaje inicial es una observación controlada, no aprendizaje
automático ni virtualización universal.

### Datos de identificación

Al registrar una aplicación, PM obtiene candidatos a partir de:

-   Nombre de la aplicación.
-   Nombre del ejecutable.
-   `CompanyName`, `ProductName`, `FileDescription` y otros metadatos PE
    disponibles.
-   Fabricante o publisher conocido.
-   Nombre de la carpeta y del paquete.
-   Alias introducidos por el usuario.

Estos datos se guardan en la sección de la aplicación dentro de
`pm.ini`.

### Primera ejecución

1.  PM identifica rutas y claves candidatas.
2.  Registra el estado inicial de los candidatos.
3.  Prepara respaldos de las rutas exclusivas que puedan ser
    modificadas.
4.  Inicia la aplicación en modo aprendizaje.
5.  Observa los cambios durante la ejecución.
6.  Al cerrar, compara el estado con el baseline.
7.  Clasifica los cambios por archivos, directorios y registro.
8.  Genera una receta preliminar.
9.  Limpia los cambios atribuibles y restaura los respaldos.
10. Guarda la receta en `pm.ini` solo si puede describir las operaciones
    con suficiente certeza.

La barra blanca de la píldora representa el aprendizaje inicial.

### Límites del aprendizaje

-   La observación no garantiza descubrir todas las escrituras.
-   Una notificación de cambio no contiene necesariamente el contenido
    anterior.
-   Los cambios ambiguos no se eliminan automáticamente.
-   Una aplicación sin receta suficientemente segura permanece blanca/no
    verificada.
-   Una receta se revisa en ejecuciones posteriores y puede ampliarse si
    aparecen nuevas rutas atribuibles.

La primera ejecución de una aplicación desconocida es una fase de
aprendizaje con garantías limitadas. PM no debe presentarla como una
aplicación ya verificada.

## 6. Detección y tratamiento del registro

### Identificación de claves candidatas

La primera aproximación será registrar claves HKCU relacionadas con:

-   Nombre de la aplicación.
-   Nombre del ejecutable.
-   Fabricante/publisher.
-   Alias conocidos.

Ejemplos conceptuales:

``` text
HKCU\Software\<Publisher>\<Application>
HKCU\Software\<Application>
```

Si la clave no existe, PM puede observar cambios de nombres bajo el
padre candidato y comparar antes/después.

### Qué puede hacer RegNotifyChangeKeyValue

`RegNotifyChangeKeyValue` permite recibir avisos cuando cambia una clave
y, si se solicita, sus subclaves. No identifica por sí solo qué proceso
produjo el cambio ni entrega un snapshot del contenido anterior.

Por eso PM debe:

1.  Registrar el baseline de las claves candidatas antes de ejecutar.
2.  Supervisar cambios en esas ramas.
3.  Volver a leer y comparar las ramas después.
4.  Guardar el estado original completo necesario para restaurar.
5.  Incluir en la receta solo las claves atribuibles con suficiente
    confianza.

No se realizará un snapshot indiscriminado de todo HKCU.

### Cambios de Windows fuera del nombre de la aplicación

Windows puede guardar rastros en ubicaciones comunes que no incluyen el
nombre del fabricante o de la aplicación, como áreas de Recent, Explorer
o compatibilidad.

PM no debe limpiar esas áreas globales por inferencia. Solo podrá
hacerlo mediante adaptadores conocidos y recetas exactas, con baseline
previo y atribución comprobada.

Si no puede atribuir el cambio, lo deja sin tocar y la aplicación no
obtiene clasificación verde.

### Transacción de registro

Para cada raíz incluida en la receta:

1.  Capturar recursivamente el estado original, incluidos nombres, tipos
    y datos de valores, subclaves y valores descendientes.
2.  Registrar si la clave existía antes.
3.  Aplicar la configuración portable si corresponde.
4.  Ejecutar la aplicación.
5.  En cierre normal, persistir los cambios portables en `Data` del USB.
6.  Eliminar la clave temporal de sesión.
7.  Restaurar el estado original del anfitrión.
8.  Verificar el resultado.

Alcance inicial: HKCU y rutas explícitas. No tocar HKLM ni raíces
globales.

## 7. PMGuard (PMG)

PMGuard es una instancia independiente de la función de cierre de
emergencia de PM. No es un segundo gestor de aplicaciones ni un
coordinador de configuración.

### Responsabilidades

-   Permanecer disponible mientras PM está operativo.
-   Vigilar la terminación de PM.
-   Leer el manifiesto temporal de la sesión si PM desaparece.
-   Ejecutar la misma rutina de emergencia que PM.
-   No mantener historiales ni informes.

### Relación entre PM y PMG

-   PM es el proceso principal y propietario de las operaciones.
-   PMG es una sombra de emergencia.
-   PM y PMG utilizan el mismo código de limpieza/restauración.
-   PM escribe el manifiesto de la sesión en `%TEMP%`.
-   PM lee ese manifiesto durante el cierre normal.
-   Si PM desaparece, PMG lee el mismo manifiesto.
-   PMG no necesita recibir ni almacenar una copia del `pm.ini`.
-   PML abre la sesión y arranca un único PMG, también con el manifiesto vacío.
    Al cerrar una aplicación, PML actualiza sus operaciones y mantiene el
    guardián para el resto de la sesión.

No se necesita una tubería de mensajes para transferir recetas. La
receta permanente está en el USB; el manifiesto de sesión está en
`%TEMP%`.

### Manifiesto de sesión en TEMP

Ejemplo conceptual:

``` text
%TEMP%\PortableManager\Session\<SessionID>\session.ini
```

Contiene la información operativa de todas las aplicaciones controladas por la sesión activa:

-   Aplicación y transacción.
-   Rutas originales.
-   Rutas `.pmbkp`.
-   Directorios de trabajo creados.
-   Claves exactas del registro.
-   Estado de cada operación.
-   Procesos/Job Objects asociados.
-   Operaciones inversas pendientes.

No es un historial ni un backup de los datos portables.

PM escribe el manifiesto de forma atómica antes de cada operación
destructiva y actualiza el estado después de completarla. PMG lee el
mismo archivo si debe actuar.

Los identificadores de Job Objects son recursos del sistema operativo y
no se sustituyen por simples números de PID. La implementación debe
compartir o abrir los objetos de kernel de forma segura para que ambos
procesos puedan terminar los grupos.

### Supervisión mutua

-   PMG espera la terminación del proceso PML. Si PML desaparece, PMG
    restaura todo lo pendiente desde el manifiesto de la sesión.
-   PML detecta la terminación inesperada de PMG con una espera asíncrona
    sobre su handle. Mantiene las aplicaciones abiertas e intenta reponer
    el guardián en la misma sesión. Los intentos se cancelan al cerrar PML.
-   Hay tres intentos como máximo: inmediato, dos segundos después del
    primer fallo y cinco segundos después del segundo. El escudo de PMD
    está verde cuando PMG está listo. El escudo y el LED de la píldora
    están rojos mientras falta PMG, incluso durante la recuperación.
    Si se agota el presupuesto, el rojo permanece hasta el próximo arranque.
-   Los Job Objects con `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` pueden
    servir como última protección para terminar procesos si se pierden
    todos los handles.

Si PM y PMG desaparecen simultáneamente, o Windows se apaga
abruptamente, no se promete restauración. Esos casos quedan fuera del
alcance operativo previsto de Lite.

## 8. Rutinas de cierre

### Cierre normal

1.  Bloquear nuevos lanzamientos.
2.  Solicitar cierre normal a las aplicaciones/lanzadores.
3.  Permitir que PAF ejecute su rutina propia.
4.  Esperar el cierre de todos los procesos del Job Object.
5.  Guardar en el USB los datos portables que correspondan.
6.  Ejecutar la receta PM.
7.  Eliminar los datos de trabajo temporales.
8.  Restaurar respaldos `.pmbkp` y claves originales.
9.  Verificar la restauración.
10. Eliminar el manifiesto temporal y cerrar PM/PMG.

### Cierre rápido

Mismo resultado que el cierre normal, pero con un tiempo de espera
reducido. Si la aplicación no termina dentro del plazo, PM fuerza el
cierre y continúa con la limpieza.

### Rutina única de emergencia

Se ejecuta ante:

-   Cierre crítico solicitado por el usuario.
-   Desconexión del USB.
-   Caída o terminación inesperada de PM.
-   Pérdida inesperada de PMGuard mientras PM continúa vivo.

Secuencia:

1.  Bloquear nuevos lanzamientos.
2.  Terminar inmediatamente los Job Objects y sus procesos
    descendientes.
3.  No esperar la rutina normal de cierre de PAF ni de aplicaciones ZIP.
4.  Eliminar los directorios de trabajo creados por PM.
5.  Eliminar claves temporales creadas o aplicadas por PM.
6.  Restaurar respaldos originales de archivos, directorios y registro.
7.  Verificar lo posible con los recursos locales disponibles.
8.  Cerrar PM y PMG.

En emergencia no se intenta guardar el estado interno de la aplicación
en el USB. La prioridad es restaurar el anfitrión.

No se generan reportes, historiales ni notificaciones de limpieza.

## 9. Job Objects

Usar Job Objects nativos de Windows para agrupar los procesos de cada
aplicación.

-   Un Job Object por instancia de aplicación.
-   El lanzador y sus descendientes deben quedar dentro del grupo.
-   PM y PMG deben poder terminar el mismo grupo.
-   `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` se utiliza como protección
    adicional, no como sustituto de la rutina de restauración.

La aplicación decide si abre una o varias ventanas; PM administra el
grupo completo.

## 10. Colores y estado de aplicaciones

  ---------------------------------------------------------------------
  Estado                             Significado
  ---------------------------------- ----------------------------------
  Blanco/gris                        Aplicación aún no aprendida o
                                     receta no verificada

  Verde                              Sin residuos detectados dentro del
                                     alcance comprobado

  Amarillo                           Requiere limpieza PM conocida y
                                     verificable

  Rojo                               Quedan residuos, hay conflictos o
                                     no se puede restaurar con
                                     seguridad
  ---------------------------------------------------------------------

Los colores expresan el resultado del alcance auditado; no garantizan
ausencia absoluta de rastros en Windows.

## 11. Referencias técnicas y proyectos estudiados

### PortableApps.com Launcher

Se adopta como referencia para recetas de movimiento,
respaldo/restauración y registro:

-   `[FilesMove]` y `[DirectoriesMove]` respaldan un destino
    preexistente y lo restauran al finalizar.
-   `[DirectoriesCleanupIfEmpty]` elimina padres solo cuando quedan
    vacíos.
-   `[RegistryKeys]` conserva claves portables y restaura la clave
    original del anfitrión.
-   `[RegistryCleanupIfEmpty]` evita dejar claves padre vacías.

Documentación: -
https://portableapps.com/manuals/PortableApps.comLauncher/ref/launcher.ini/filesystem.html -
https://portableapps.com/manuals/PortableApps.comLauncher/ref/launcher.ini/registry.html -
https://portableapps.com/manuals/PortableApps.comLauncher/topics/registry.html

### Cameyo y ThinApp

Se toma como referencia conceptual la separación entre cambios
persistentes de la aplicación y cambios temporales del anfitrión. PM no
implementa sus motores de virtualización.

### Turbo Studio y Sandboxie-Plus

Se mantienen como referencias de virtualización más profunda, pero no se
adopta su arquitectura de drivers, servicios o interceptación universal.

### API de Windows

-   `RegNotifyChangeKeyValue`:
    https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regnotifychangekeyvalue
-   `ReadDirectoryChangesW`:
    https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw
-   Job Objects:
    https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects

`ReadDirectoryChangesW` puede perder eventos si se desborda su búfer;
ante esa condición PM debe volver a enumerar y comparar la ruta
observada. Las notificaciones de registro informan cambios, no
identifican por sí mismas el proceso responsable.

## 12. Fuera del alcance de Lite

-   Virtualización universal del filesystem o registro.
-   Drivers, servicios y hooks globales.
-   Machine learning para crear recetas automáticamente.
-   Cifrado Secure de los datos.
-   Sincronización Cloud/rclone.
-   Recuperación de perfiles de aplicaciones tras desconexión USB.
-   Garantía frente a corte eléctrico, apagado abrupto de Windows o
    terminación simultánea de PM y PMG.
-   Limpieza de rutas o claves cuya procedencia no pueda atribuirse.

## 13. Criterios de aceptación antes de implementar la interfaz

1.  Una receta nativa puede dirigir datos al USB sin modificar el host.
2.  El lanzador PM puede aplicar variables de entorno solo al proceso y
    sus descendientes.
3.  El mecanismo `.pmbkp` restaura carpetas existentes y conserva
    correctamente la ausencia original.
4.  Las carpetas compartidas no se mueven completas.
5.  El aprendizaje compara baseline y estado posterior sin borrar
    cambios ambiguos.
6.  La transacción de registro restaura valores, tipos, datos y
    subclaves originales.
7.  PM y PMG pueden leer el mismo manifiesto temporal.
8.  Si PM desaparece, PMG ejecuta la rutina de emergencia.
9.  Si PMG desaparece, PML conserva la sesión e intenta restablecer su guardián.
10. La rutina de emergencia no depende del USB.
11. El cierre normal permite que los lanzadores PAF terminen su propia
    limpieza.
12. El cierre crítico no espera a los lanzadores y prioriza restaurar el
    anfitrión.
13. El manifiesto TEMP se elimina después de una limpieza verificada.
14. No se generan historiales ni informes de sesión.

**Principio final:** PM conserva en el USB la configuración y los datos
portables; en el anfitrión solo mantiene el runtime y la información
temporal imprescindible para deshacer los cambios que PM mismo autorizó.
