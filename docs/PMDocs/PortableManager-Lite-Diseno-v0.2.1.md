# PortableManager Lite --- Continuación del diseño funcional

**Versión:** 0.2.1 (revisión de alineación y decisiones operativas)\
**Fecha:** 11 de noviembre de 2026\
**Estado:** diseño acordado; sin implementación.\
**Documento base:** `PortableManager-Lite-Diseno-v0.1-base.md`.

> **Regla de interpretación:** este documento **complementa y, cuando
> hay diferencias, sustituye** las decisiones de v0.1. No describe
> funciones ya implementadas. PortableManager (PM) es la aplicación;
> PM-Desktop (PMD) es solo su vista temporal. Contraer PMD **nunca**
> equivale a cerrar PM.

## 18. Principio de producto y alcance

-   PM Lite es un entorno portátil, pequeño, discreto y de intervención
    mínima. Su propósito es permitir usar aplicaciones y documentos del
    USB conservando sus datos allí y **reduciendo al mínimo los rastros
    de información personal en Windows anfitrión**, particularmente en
    entornos compartidos como cibercafés.
-   PM **no** es un sistema de diagnóstico, auditoría ni administración
    general de Windows. No presenta asistentes, informes ni advertencias
    por cada incidencia. El usuario decide si restablece, reinstala o
    elimina una aplicación; PM no borra automáticamente sus **datos
    portables** porque una aplicación falle.
-   Se aprovechan comportamientos ya resueltos por PortableApps.com. Los
    lanzadores **PAF oficiales administran su propia portabilidad y
    limpieza**; PM no les superpone un segundo motor de restauración. PM
    gestiona la portabilidad de aplicaciones instaladas desde
    ZIP/7z/RAR/TAR.
-   La obligación de limpiar **residuos del anfitrión** al terminar una
    aplicación es distinta de borrar deliberadamente los **datos
    personales guardados en el USB**. La primera es automática según el
    ciclo de vida; la segunda únicamente ocurre cuando el usuario ordena
    Restablecer o Desinstalar.
-   No añadir IA, cifrado, nube, controladores o diagnósticos complejos
    a Lite. No garantizar ausencia absoluta de trazas en todos los
    hosts: sin privilegios elevados no se pueden identificar o revertir
    todas las escrituras de terceros.

## 19. Instalación contextual y actualizaciones

### 19.1 Menú de destinos sin clic redundante

Cuando el portapapeles contiene un paquete compatible, la píldora
propone instalarlo. El nombre mostrado es un **destino predeterminado**;
puede pulsarse para elegir otro:

1.  **Clic en icono instalar:** comienza inmediatamente la instalación o
    actualización del destino propuesto.
2.  **Clic en el nombre:** abre una lista contextual de instalaciones
    existentes que pueden ser destino, más `+ Nueva instalación`.
3.  **Clic en una instalación de la lista:** **comienza inmediatamente**
    la actualización de esa instalación; no se vuelve a pulsar instalar.
4.  **Clic en `+ Nueva instalación`:** abre el selector de carpeta;
    confirmar la carpeta, existente o recién creada, **inicia
    inmediatamente** la instalación.
5.  Ignorar o cancelar el menú no altera archivos.

El usuario puede elegir conscientemente actualizar una instalación
distinta de la sugerida. Los nombres de archivos versionados
(`Firefox_v02.zip`, etc.) orientan la propuesta, pero no son evidencia
suficiente de identidad.

### 19.2 Instalador PAF frente a comprimidos

-   **PAF:** ejecutar su **instalador oficial** sobre la instalación
    elegida, respetando el lanzador portable y sus reglas de
    actualización. Su lanzador oficial es el ejecutable predeterminado.
-   **ZIP/7z/RAR/TAR:** extraer primero en una carpeta de trabajo
    `App/.temp/<id-operación>/` ---por ejemplo, para FirefoxPortable---,
    y solo después copiar/sobrescribir el destino elegido. La extracción
    incompleta **nunca** debe convertirse en una instalación parcial.
-   Para los ZIP no se implementa en Lite un clasificador sofisticado de
    archivos de perfil. Se respeta la estructura del paquete y se
    conserva el perfil externo administrado por PM. Un ZIP que contenga
    un archivo de configuración en el propio directorio del programa
    **podría sobrescribirlo**: no prometer conservación universal de
    contenidos mezclados.
-   El color del **icono de instalación** es **verde** si puede
    iniciarse la operación y **rojo** si, por ejemplo, la aplicación
    destino está activa o sus archivos no pueden actualizarse. El color
    comunica disponibilidad, **no seguridad**. Una aplicación activa no
    se cierra automáticamente para actualizar.
-   Se admite detección de errores y eliminación/reversión de elementos
    creados por una instalación fallida, dentro de lo técnicamente
    alcanzable; no introducir diagnósticos ni asistentes. La barra de
    instalación empieza verde y cambia a roja mientras se revierte un
    error.

### 19.3 Ejecutable principal y criterio de selección

-   En PAF se utiliza el **lanzador PortableApps**, no el EXE interno de
    `App/`.
-   En un ZIP con **un único EXE en la raíz**, se registra
    automáticamente como ejecutable principal.
-   En un ZIP con **varios EXE en la raíz**, PM ofrece selección de
    ejecutable principal; la elección queda en el INI y puede editarse
    después.
-   El usuario decide si el paquete es portable; PM no certifica ni
    diagnostica su funcionamiento.
-   **Solo archivos `.exe` se registran como aplicaciones en Lite.**
    BAT/CMD/PS1/PY podrán aparecer como archivos, pero no se registran
    como ejecutables principales de aplicación. Los comandos
    configurables por INI son una categoría distinta y la interfaz
    visual para crearlos queda para futuras versiones.

## 20. Una sola carpeta App: aplicaciones instaladas y carpetas manuales

Estructura física simplificada:

``` text
<USB>/
├── pm.exe
├── App/
│   ├── .temp/
│   ├── FirefoxPortable/
│   ├── NotepadPlusPlusPortable/
│   └── MiPrograma/                 # Copiada manualmente, todavía no registrada
├── Data/                            # Estado propio de PM y perfiles ZIP capturados
└── Other/                           # Archivos personales visibles como carpeta en PMD
```

**No existen carpetas físicas separadas para aplicaciones registradas y
pendientes.** PMD interpreta el registro:

-   Carpeta de `App/` **registrada** → se muestra su **icono de
    aplicación** directamente en PMD; no se muestra como carpeta.
-   Carpeta de `App/` **no registrada** → se muestra como carpeta; clic
    sencillo entra y permite explorar.
-   Dentro de una carpeta no registrada: **clic sencillo en un EXE =
    probar/ejecutar sin registrar**; **doble clic en un EXE = registrar
    y ejecutar**. El usuario puede decidir libremente si registra una
    carpeta.
-   `Other/` se muestra como carpeta normal de PMD y se explora/abre
    mediante el comportamiento ya definido en v0.1.
-   Una vez registrada, la app se muestra como icono y **clic sencillo
    la ejecuta**.
-   PM siempre invoca el **lanzador registrado**. El propio programa
    decide si reutiliza una ventana, abre instancias múltiples o las
    bloquea; PM no impone reglas de instancia.

### 20.1 Nombre predeterminado y búsqueda

El nombre por defecto procede del EXE elegido **sin `.exe`**:
`MiPrograma64.exe` → `MiPrograma64`. **Ese nombre siempre sirve para
buscar y ejecutar** (MagicWord); no puede quedar sin identidad de
búsqueda. Los alias son **adicionales, opcionales y editables** en el
INI; no sustituyen la búsqueda por nombre.

``` ini
[App.MiPrograma]
Name=MiPrograma64
Executable=App/MiPrograma/MiPrograma64.exe
Aliases=
```

Si una carpeta registrada desaparece manualmente, **el icono permanece
gris**, sin poder ejecutarse. Desde su menú contextual, **Desinstalar**
ejecuta la limpieza disponible y retira la información asociada. Si
reaparece antes de desinstalar, puede recuperarse su identidad.

## 21. Estados visuales: tres conceptos distintos

### 21.1 Punto del icono de aplicación (estado de privacidad)

  ---------------------------------------------------------------------
  Punto                              Significado
  ---------------------------------- ----------------------------------
  **Blanco/gris**                    **Sin verificar**; son el mismo
                                     estado gráfico, no dos categorías.

  **Verde**                          La aplicación, al cerrarse, **no
                                     deja residuos observados** que
                                     requieran limpieza adicional de
                                     PM.

  **Amarillo**                       Necesita la **limpieza estándar de
                                     PM** y, después de ella, el host
                                     queda restaurado respecto de los
                                     cambios controlados.

  **Rojo**                           **Persisten residuos** tras la
                                     limpieza aplicable de PM; se
                                     anotan en el log del USB cuando
                                     esté disponible.
  ---------------------------------------------------------------------

La categoría se determina por comprobación, **no por ser PAF o ZIP**. Un
PAF no es automáticamente verde; su propio lanzador puede dejar residuos
observados. PM no repite sobre PAF la limpieza que le corresponde al
lanzador.

### 21.2 Icono contextual de instalación

-   **Verde:** operación disponible.
-   **Rojo:** operación no disponible ahora, por ejemplo porque el
    destino está ejecutándose.

No confundir este icono con el punto de privacidad de la aplicación.

### 21.3 LED y barra de la píldora (estado de PM)

  ---------------------------------------------------------------------
  Señal                              Significado
  ---------------------------------- ----------------------------------
  **LED habitual de reposo**         PM está activo, sin que el LED
                                     exprese el nivel de privacidad de
                                     una app particular.

  **Barra blanca**                   **Primera ejecución/aprendizaje**
                                     de un ZIP desconocido. Avance solo
                                     cuando una fase sea realmente
                                     medible; no inventar porcentajes
                                     mientras el usuario mantiene la
                                     app abierta.

  **Barra verde**                    **Instalación/actualización** y
                                     **cierre normal de PM**, con
                                     progreso medido donde sea posible.

  **Barra roja tras empezar verde**  **Error de instalación**;
                                     permanece roja mientras se
                                     eliminan/revierten residuos
                                     controlados de la instalación.

  **Barra roja**                     **Cierre forzado** solicitado por
                                     el usuario.

  **LED rojo**                       Pérdida del **USB o fuente de
                                     datos**; dispara cierre de
                                     emergencia.
  ---------------------------------------------------------------------

El LED global y la clasificación de un icono de app son independientes.
Contraer PMD no inicia el cierre ni muestra barra de cierre.

## 22. Motor de portabilidad para aplicaciones ZIP

### 22.1 Responsabilidades

-   **Aplicaciones PortableApps PAF:** confiar su ciclo de
    portabilidad/restauración al **lanzador oficial**. PM puede observar
    procesos y comprobar resultados de forma **no destructiva**, sin
    duplicar las recetas del lanzador.
-   **Aplicaciones importadas desde comprimidos:** PM descubre, aprende,
    mueve y restaura los recursos locales mediante recetas propias.
-   El observador no se extiende a todo el disco, no requiere servicios
    persistentes, drivers, elevación ni IA.

### 22.2 Ciclo acordado: renombrar → preparar → ejecutar → guardar → restaurar

Este ciclo se aplica a aplicaciones ZIP cuando las prioridades 1 y 2 no
permiten que la aplicación utilice directamente sus datos en el USB.

1.  **Identificar la ruta exacta:** PM utiliza la ruta aprendida y
    verifica que corresponda a datos exclusivos de esa aplicación. No
    trata una carpeta de fabricante compartida como si fuera exclusiva.
2.  **Preservar el anfitrión:** si la carpeta exclusiva existe, PM la
    renombra en el mismo volumen añadiendo `.pmbkp` (por ejemplo,
    `MiPrograma` → `MiPrograma.pmbkp`). Si no existía, registra
    expresamente esa ausencia. Nunca sobrescribe un `.pmbkp` previo:
    ante una colisión, no modifica la ruta y marca la operación como no
    disponible.
3.  **Preparar la sesión:** PM crea la ruta de trabajo y coloca allí los
    datos portables persistidos en `Data/apps/<id-estable>/` del USB. En
    una primera ejecución sin datos portables, prepara una ruta de
    trabajo vacía.
4.  **Ejecutar:** PM inicia el EXE registrado y supervisa los procesos
    que pertenecen a la aplicación. El lanzador o la propia aplicación
    decide si permite varias instancias.
5.  **Guardar en cierre normal:** una vez terminados los procesos, PM
    guarda en el USB los datos portables nuevos o modificados que sean
    atribuibles a la aplicación.
6.  **Restaurar el anfitrión:** PM elimina la ruta de trabajo de la
    sesión y devuelve `.pmbkp` a su nombre original. Si la ruta no
    existía antes, elimina la ruta de trabajo y deja el anfitrión sin
    esa carpeta.

Reglas de seguridad:

-   Si el padre del fabricante es compartido, PM trabaja únicamente
    sobre la subcarpeta exclusiva de la aplicación.
-   Si no puede identificar un límite exclusivo y seguro, no mueve la
    carpeta compartida ni promete limpiar esa ubicación.
-   Si una ubicación contiene datos de varias aplicaciones, PM no la
    renombra completa.
-   Si la carpeta exclusiva contiene archivos de la aplicación y datos
    de usuario mezclados, PM no presupone que puede separarlos; la
    receta debe definir qué datos se conservan.
-   PM no sigue junctions, enlaces simbólicos ni otros puntos de
    reanálisis al eliminar una ruta de trabajo sin validarlos
    expresamente.
-   Las carpetas de instalación bajo `App/` no son objetivos de esta
    limpieza.

Para el registro se utiliza una transacción equivalente, no un
renombrado de claves: PM captura el estado original de la clave HKCU
exacta, aplica los datos portables de la aplicación, guarda los cambios
en el USB durante un cierre normal y restaura el estado original del
anfitrión.

**Datos portables que se conservan:** `%APPDATA%`, `%LOCALAPPDATA%` y
ajustes asociados del registro que puedan identificarse con suficiente
certeza. No se separan automáticamente cachés y preferencias. Las
exclusiones podrán definirse por receta. Los temporales redirigidos se
descartan según las reglas de la aplicación.

**Cierre crítico:** no guarda los cambios de la sesión que todavía no
hayan sido persistidos en el USB. La prioridad es terminar los procesos
y restaurar el anfitrión.

## 22.3 Primera ejecución: aprendizaje sin IA

-   La primera apertura de un ZIP desconocido puede ser una **ejecución
    de prueba normal**. PM obtiene el inventario inicial y observa
    mientras el usuario usa el programa; **no lo cierra
    automáticamente**.
-   Al cerrarse, PM compara el estado anterior/posterior en las
    ubicaciones acotadas: `%APPDATA%`, `%LOCALAPPDATA%`,
    `%TEMP%`/`%TMP%`, `HKCU\Software` y rutas conocidas pertinentes.
    Considera nombres de ejecutable/aplicación como **pistas**, sin
    limitarse a ellos.
-   El inventario previo/posterior, el seguimiento dirigido y las
    recetas **son complementarios**, no opciones excluyentes. No es
    necesario vigilar todo Windows en tiempo real.
-   PM guarda en el USB los datos nuevos atribuibles a la aplicación,
    limpia lo que pueda verificar y **aprende las rutas** para el ciclo
    completo de las siguientes ejecuciones.
-   **Limitación aceptada:** si en la primera ejecución una app modifica
    contenido preexistente que PM no respaldó antes, un snapshot
    posterior no permite restaurarlo fielmente; no se borra a ciegas. El
    aprendizaje no equivale a identificación perfecta del proceso autor
    de cada cambio.
-   La barra blanca de la píldora señala la fase de aprendizaje. Si
    durante la app abierta no puede medirse una fracción real, no se
    presenta un porcentaje fingido. El procesamiento final muestra
    progreso real cuando existe una unidad de trabajo contable.

### 22.4 Repeticiones, procesos y conflictos

-   Aunque el usuario invoque una app diez veces, PM mantiene **una
    identidad y receta por aplicación**, no diez perfiles.
-   Antes de restaurar recursos compartidos, esperar que terminen los
    procesos identificados que todavía los utilizan. No alterar
    decisiones del lanzador sobre múltiples ventanas o instancias.
-   Si dos apps gestionadas comparten una ubicación protegida, **no
    restaurarla mientras la otra la necesita**. La coordinación ha de
    limitarse a recursos identificados; las colisiones no observadas
    siguen siendo una restricción técnica.
-   Las modificaciones de otras aplicaciones del anfitrión no deben
    atribuirse automáticamente a PM solo por coincidir en el tiempo.

## 23. Cierre y limpieza

### 23.1 Cierre ordinario de una aplicación

-   Al cerrar desde PMD o **desde la propia X de la aplicación**, PM
    detecta que finalizaron sus procesos y ejecuta el cierre/limpieza
    que corresponda.
-   **Verde:** no necesita limpieza adicional de PM.
-   **Amarillo:** PM ejecuta limpieza estándar y restaura las
    ubicaciones que controla; debe terminar sin residuos observados
    pendientes.
-   **Rojo:** PM ejecuta limpieza normal y conserva en el log del USB
    los residuos que permanezcan.
-   La limpieza es **híbrida**: tras terminar la aplicación se limpian
    sus recursos relevantes; al cerrar PM se revisan únicamente
    operaciones pendientes. No repetir un trabajo que ya se completó.
-   **Cerrar PMD solo oculta la interfaz**, no termina procesos ni
    ejecuta limpieza global.

### 23.2 Cierre forzado de aplicación

-   El **cierre forzado individual** se invoca **solo desde el menú
    contextual de una aplicación roja en PMD**.
-   Forzado significa **terminar procesos resistentes** y retirar de
    manera más agresiva los elementos **atribuidos como creados durante
    la sesión**, además de restaurar recursos originales respaldados.
-   **No** significa eliminar indiscriminadamente claves antiguas o
    archivos compartidos del anfitrión. Debe respetar la propiedad y
    preexistencia de datos; algunas operaciones pueden no ser posibles
    sin permisos elevados.
-   Cerrar normalmente una app mediante su propia X **siempre** implica
    cierre normal; no inicia forzado de manera implícita.

### 23.3 Cierre normal/rápido/crítico de PM

Se conservan las opciones de cierre global desde la píldora definidas en
v0.1. Contraer PMD nunca cierra PM.

-   **Normal:** solicita el cierre ordenado de las aplicaciones, permite
    que los lanzadores PAF ejecuten su rutina propia, espera los
    procesos, guarda los datos portables ZIP en el USB y restaura el
    anfitrión.
-   **Rápido:** reduce el tiempo de espera, pero no omite la
    restauración de los recursos del anfitrión que PM ya modificó. Si
    debe terminar procesos por tiempo agotado, la persistencia de los
    últimos cambios de la aplicación puede quedar incompleta.
-   **Crítico:** tiene prioridad sobre cualquier cierre normal de
    PortableApps o de una aplicación ZIP. No espera sus rutinas de
    cierre: termina inmediatamente los procesos bajo responsabilidad de
    PM y ejecuta la restauración del anfitrión con el manifiesto local
    de sesión. No intenta guardar en el USB los cambios de aplicación
    que aún no estén persistidos.

En cierre crítico se acepta que los datos de trabajo de la aplicación
que no se hayan guardado previamente en el USB puedan perderse. PM no
afirma que un lanzador PAF haya completado su propia limpieza si fue
interrumpido. PM restaura los cambios que controla y que dejó
registrados; cualquier residuo no atribuible o no restaurable queda
fuera de la garantía.

El cierre normal utiliza barra verde; las acciones de cierre forzado
utilizan barra roja. Los tiempos exactos y las medidas de avance externo
se validarán técnicamente.

### 23.4 Desconexión del USB o de la fuente de datos

-   Ante pérdida de fuente, **LED rojo y cierre de emergencia
    automático**, sin diálogo ni espera de reconexión.
-   Si PM sigue operativo, ejecuta la rutina crítica inmediatamente. Si
    PM desaparece inesperadamente, PMGuard (PMG) lee el manifiesto
    temporal y ejecuta esa misma rutina.
-   PM y PMG utilizan un manifiesto de sesión local en `%TEMP%`. PM lo
    escribe antes de cada modificación del anfitrión; PM lo lee durante
    su propio cierre y PMG solo lo utiliza si PM ya no está disponible.
-   La receta permanente de la aplicación y los datos portables
    continúan en el USB. El manifiesto TEMP contiene las operaciones
    concretas pendientes de restauración, no un historial ni una copia
    de los perfiles.
-   Si PMG termina mientras PM sigue vivo, PM continúa como propietario
    de la sesión. Puede reiniciar PMG, pero su desaparición no provoca
    por sí sola un cierre de emergencia.
-   El runtime local de PM/PMG y el manifiesto en TEMP deben prepararse
    antes de iniciar aplicaciones. Que el cierre continúe funcionando
    tras retirar el USB es un objetivo sujeto a prueba técnica.
-   **No se dejan logs, respaldos ni informes persistentes en el
    anfitrión.** El manifiesto TEMP y los snapshots estrictamente
    necesarios para restaurar son transitorios y se eliminan después de
    una restauración verificada. Los registros de incidencias que se
    decida conservar residen en el USB y solo se escriben cuando está
    disponible.
-   Cuando la fuente reaparece, una nueva ejecución parte de la última
    configuración y los últimos datos que realmente quedaron guardados
    en el USB. Los cambios no guardados pueden perderse.
-   RAM ordinaria no garantiza exclusión del archivo de paginación. Si
    PM y PMG desaparecen simultáneamente, Windows se apaga abruptamente,
    se pierde el estado local o faltan permisos, no se garantiza la
    restauración completa.
-   Sin fuente de datos, el flujo final es: cerrar, restaurar cuanto sea
    posible con los recursos locales y terminar.

## 24. Operaciones del menú contextual: Restablecer, Archivar, Desinstalar

**El flujo habitual de instalación sirve también para actualizar o
reinstalar.** No añadir acciones separadas «Reparar» o «Actualizar» al
menú contextual.

  ---------------------------------------------------------------------------------
  Acción            Programa en Datos         Registro/receta   Durante ejecución
                    `App/`      portables en  PM                
                                USB                             
  ----------------- ----------- ------------- ----------------- -------------------
  **Restablecer**   Se conserva **Se          **Se conserva**   **Deshabilitada**
                                eliminan**                      

  **Archivar**      **Se        **Se          **Se conserva**   **Deshabilitada**
                    elimina**   conservan**                     

  **Desinstalar**   **Se        **Se          Se retira del     Solicita cerrar,
                    elimina**   eliminan**    registro activo   limpia y desinstala
                                              tras limpieza     
  ---------------------------------------------------------------------------------

### 24.1 Restablecer

-   Solo cuando la aplicación **no está en uso**; la opción aparece en
    gris cuando está ejecutándose.
-   Elimina los datos **portables de la aplicación en el USB**, no el
    ejecutable ni la receta de limpieza aprendida. Conserva nombre,
    identificador y alias.
-   No borra datos automáticamente por fallos, ni diagnostica errores.
-   Como la acción elimina datos personales, puede requerir una
    confirmación mínima; el detalle exacto de confirmación no ha sido
    cerrado.

### 24.2 Archivar

-   Solo cuando la aplicación está cerrada.
-   Elimina sus **binarios** para ahorrar espacio, conserva **datos
    portables, identidad, alias y receta**.
-   El icono **desaparece de PMD** para ahorrar espacio visual.
-   Sigue disponible en la **búsqueda de la píldora** como
    `Nombre (archivado)`, pero **no es ejecutable**.
-   Seleccionarla desde la búsqueda **abre el selector del paquete ZIP o
    PAF** y reutiliza el flujo de instalación. Al reinstalar, PM
    recupera los datos conservados.
-   PAF: como `Data/` suele estar dentro de `App/<AppPortable>/`,
    **mover esa `Data/` a un área de archivo del USB** antes de retirar
    binarios, y devolverla al reinstalar. ZIP: conservar perfiles ya
    separados bajo `Data/` de PM. El destino físico final de archivos
    PAF se fijará al implementar.

### 24.3 Desinstalar

-   Ejecuta cierre normal de los procesos que pertenezcan a la app,
    limpieza de residuos del anfitrión y eliminación de
    archivos/programa/datos/registro, dentro de los límites de acceso.
-   La misma acción aplica a iconos **grises por carpeta faltante**: aún
    debe ejecutar la receta o las restauraciones pendientes antes de
    retirar la asociación, sin pretender borrar una carpeta que ya no
    existe.
-   No confundir un icono gris por **carpeta faltante** con una
    aplicación **archivada**: las archivadas no se muestran en PMD y sí
    se distinguen por `(archivado)` en búsqueda.
-   No eliminar recetas o referencias imprescindibles antes de intentar
    resolver limpiezas pendientes. Si se requiere conservar un registro
    técnico residual, debe residir en el USB, no en el anfitrión.

### 24.4 Receta independiente de los datos

La receta es **configuración de PM**, no datos de usuario de la
aplicación. Por eso **Restablecer no la borra**; Archivar también la
conserva. Las exclusiones de carpetas y las rutas aprendidas pueden
editarse en el INI.

## 25. Configuración y almacenamiento sugeridos (conceptuales)

La estructura física mantiene una sola carpeta `App/`. Las
responsabilidades quedan separadas:

``` text
<USB>/
├── pm.exe
├── App/
│   ├── .temp/                       # Extracción previa a instalar
│   ├── FirefoxPortable/             # PAF, conserva App/Data/Other propios
│   ├── MiPrograma/                  # ZIP registrado o carpeta manual
│   └── OtraCarpeta/                 # Sin registrar, visible como carpeta en PMD
├── Data/
│   ├── apps.ini                     # Identidad, nombres, alias, estado
│   ├── config.ini                   # Ajustes generales
│   ├── recipes/                     # Recetas permanentes por aplicación
│   ├── apps/                        # Datos portables de aplicaciones ZIP
│   │   └── id-estable/
│   │       ├── roaming/
│   │       ├── local/
│   │       └── registry/
│   ├── archived/                    # Datos preservados de PAF archivados
│   ├── logs/                        # Incidencias/residuos, solo en USB
│   └── session.journal              # Diario de transacciones de instalación/actualización
└── Other/                           # Carpeta normal de documentos
```

Hay dos mecanismos distintos y no deben confundirse:

-   `Data/recipes/` contiene la **receta permanente** aprendida o
    configurada para cada aplicación.
-   `%TEMP%\PortableManager\Session\<SessionID>\session.ini` contiene el
    **manifiesto operativo de la sesión activa**: rutas concretas,
    respaldos `.pmbkp`, claves originales, estado de operaciones y
    recursos necesarios para restaurar el anfitrión. PM lo escribe y lo
    lee; PMG lo lee solo si PM desaparece.

El manifiesto TEMP no es un historial y se elimina después de una
restauración verificada. `Data/session.journal` permanece en el USB para
las transacciones de instalación/actualización y no sustituye al
manifiesto de recuperación del anfitrión.

Ejemplo conceptual de receta permanente, no formato normativo
definitivo:

``` ini
[App.MiPrograma]
Name=MiPrograma64
Executable=App/MiPrograma/MiPrograma64.exe
Aliases=
Type=Archive

[Observe.MiPrograma]
AppData=MiPrograma
LocalAppData=MiPrograma
Registry=HKCU\Software\MiPrograma
RedirectTemp=true
; Exclusions=...     ; opcional, editado por el usuario
```

## 26. Funciones aplazadas expresamente

-   **Editor de texto integrado**, similar a Bloc de notas, para
    visualizar/editar BAT, CMD, PS1, PY y otros archivos de texto sin
    depender de software del anfitrión.
-   **Gestión visual de comandos personalizados**, con identidad de PM;
    **por ahora se gestionan mediante INI**. Los comandos no equivalen
    automáticamente a aplicaciones registradas en PMD.
-   **Restablecimiento parcial:** borrar únicamente preferencias
    conservando ciertos perfiles/documentos; Lite solo dispone de
    **Restablecer datos completo**.
-   PM Secure y PM Cloud siguen en la hoja de ruta de v0.1, fuera del
    alcance de Lite.
-   Descubrimiento/atribución de cambios más avanzado, controladores,
    sandbox, aislamiento integral de procesos y analítica profunda
    quedan fuera de la primera edición.

## 27. Pendientes reales antes de implementar

Quedan decisiones y pruebas técnicas acotadas:

1.  **Runtime y desconexión:** comprobar que PM y PMG puedan ejecutar el
    cierre desde copias locales en TEMP cuando desaparezca el USB, sin
    depender de módulos que sigan cargándose desde él.
2.  **Manifiesto TEMP:** probar escritura atómica, actualización de
    estados, exclusión mutua entre PM/PMG y recuperación tras
    terminación abrupta de PM.
3.  **Job Objects:** probar apertura del mismo objeto nombrado desde
    PMG, terminación de descendientes, procesos que intentan escapar y
    comportamiento de `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
4.  **Aprendizaje:** medir enumeración y snapshots de
    AppData/LocalAppData/registro; gestionar escrituras intercaladas y
    contenido preexistente sin borrado indiscriminado.
5.  **`.pmbkp`:** probar renombrado/restauración, ausencia original,
    colisiones, permisos, carpetas compartidas, junctions y fallos a
    mitad de transacción.
6.  **Registro:** probar captura/restauración de valores, tipos, datos y
    subclaves, además de conflictos por modificaciones concurrentes.
7.  **Conflictos entre aplicaciones:** política concreta cuando un PAF y
    un ZIP acceden al mismo recurso; no sobrescribir el trabajo de un
    lanzador oficial.
8.  **PAF en cierre crítico:** medir qué residuos puede dejar al
    interrumpir su rutina y definir qué recursos puede restaurar PM sin
    duplicar ni dañar la lógica del lanzador.
9.  **Formato y ubicación final:** INI/recetas/journal, identificación
    estable, exclusiones y migración de datos PAF archivados.
10. **Progreso medible:** límites de porcentaje real cuando un
    instalador PAF no expone progreso o el aprendizaje depende del
    cierre decidido por el usuario.
11. **Búsqueda:** prioridad de coincidencias y comportamiento de Enter
    sobre resultados `(archivado)`.
12. **Restricciones del usuario estándar:** pruebas de permisos,
    restauración del registro y procesos que no responden.
13. **Código y UI existentes:** antes de proponer crear módulos o
    interfaces, comprobar si ya existen en el proyecto. El proyecto
    sigue en fase de diseño.

## 28. Correcciones explícitas respecto de v0.1

-   Donde v0.1 decía que PMD **nunca muestra carpetas de `App/`**,
    reemplazar por: **muestra las carpetas no registradas de `App/`; las
    registradas se representan como iconos**.
-   Donde v0.1 prometía que PM «declara» residuos o reporta errores al
    usuario, reemplazar por: **limpieza automática y discreta; log solo
    en USB cuando esté disponible y sea útil; no crear reportes en el
    host**.
-   Donde v0.1 sugería varios mecanismos de limpieza para PAF,
    reemplazar por: **la limpieza corresponde al lanzador oficial; PM
    puede verificar sin duplicar acciones destructivas**.
-   Añadir **Restablecer** y **Archivar**, además de Desinstalar;
    **sin** botón independiente de Reparar/Actualizar.
-   Clasificación de privacidad **gris/blanco, verde, amarillo, rojo**
    según el resultado de limpieza, separada de LED/barra de píldora e
    icono de instalación.
-   Sustituir la política de cierre de app: **cierre natural siempre
    normal; cierre forzado individual únicamente desde el menú
    contextual de una app roja en PMD**.
-   Para pérdida del USB: **cierre de emergencia automático** con apoyo
    en estado en RAM; no esperar reconexión ni dejar registros
    persistentes en el host.

------------------------------------------------------------------------

## 29. Aclaraciones aprobadas en la revisión del 11 de noviembre de 2026

-   El cierre crítico prevalece sobre el cierre normal de PortableApps y
    de cualquier aplicación ZIP.
-   PM escribe en `%TEMP%` el manifiesto operativo que él mismo leerá al
    cerrar. PMG lee ese mismo manifiesto únicamente si PM desaparece.
-   PMG es una sombra prescindible. Su terminación no obliga a cerrar la
    sesión; PM sigue siendo responsable y puede reiniciar PMG o
    continuar sin él.
-   La receta permanente de cada aplicación permanece en
    `Data/recipes/`; el manifiesto temporal de la sesión permanece en
    `%TEMP%`.
-   El respaldo de carpetas candidatas se realiza preferentemente
    mediante renombrado en el mismo volumen con sufijo `.pmbkp`, solo
    cuando la carpeta sea exclusiva de la aplicación.
-   La prioridad de portabilización ZIP es: mecanismo nativo de la
    aplicación; variables de entorno/adaptadores del proceso; y, como
    caso borde, preparación de rutas del anfitrión con `.pmbkp`.
-   Las carpetas compartidas no se renombrarán completas. Si no existe
    una subcarpeta exclusiva y segura, PM no la modifica.

**Resumen de alcance:** la definición funcional de instalación,
registro, ejecución, aprendizaje básico, portabilidad ZIP, clasificación
de privacidad, cierres, restablecimiento, archivado y desinstalación
está consolidada. La validación técnica sigue pendiente; no se considera
que exista ya una implementación.
