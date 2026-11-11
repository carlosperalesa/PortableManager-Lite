# PortableManager Lite — Continuación del diseño funcional

**Versión:** 0.2 (anexo de decisiones posteriores a v0.1)  
**Fecha:** 10 de noviembre de 2026  
**Estado:** diseño acordado; sin implementación.  
**Documento base:** `PortableManager-Lite-Diseno-v0.1.md`.

> **Regla de interpretación:** este documento **complementa y, cuando hay diferencias, sustituye** las decisiones de v0.1. No describe funciones ya implementadas. PortableManager (PM) es la aplicación; PM-Desktop (PMD) es solo su vista temporal. Contraer PMD **nunca** equivale a cerrar PM.

## 18. Principio de producto y alcance

- PM Lite es un entorno portátil, pequeño, discreto y de intervención mínima. Su propósito es permitir usar aplicaciones y documentos del USB conservando sus datos allí y **reduciendo al mínimo los rastros de información personal en Windows anfitrión**, particularmente en entornos compartidos como cibercafés.
- PM **no** es un sistema de diagnóstico, auditoría ni administración general de Windows. No presenta asistentes, informes ni advertencias por cada incidencia. El usuario decide si restablece, reinstala o elimina una aplicación; PM no borra automáticamente sus **datos portables** porque una aplicación falle.
- Se aprovechan comportamientos ya resueltos por PortableApps.com. Los lanzadores **PAF oficiales administran su propia portabilidad y limpieza**; PM no les superpone un segundo motor de restauración. PM gestiona la portabilidad de aplicaciones instaladas desde ZIP/7z/RAR/TAR.
- La obligación de limpiar **residuos del anfitrión** al terminar una aplicación es distinta de borrar deliberadamente los **datos personales guardados en el USB**. La primera es automática según el ciclo de vida; la segunda únicamente ocurre cuando el usuario ordena Restablecer o Desinstalar.
- No añadir IA, cifrado, nube, controladores o diagnósticos complejos a Lite. No garantizar ausencia absoluta de trazas en todos los hosts: sin privilegios elevados no se pueden identificar o revertir todas las escrituras de terceros.

## 19. Instalación contextual y actualizaciones

### 19.1 Menú de destinos sin clic redundante

Cuando el portapapeles contiene un paquete compatible, la píldora propone instalarlo. El nombre mostrado es un **destino predeterminado**; puede pulsarse para elegir otro:

1. **Clic en icono instalar:** comienza inmediatamente la instalación o actualización del destino propuesto.
2. **Clic en el nombre:** abre una lista contextual de instalaciones existentes que pueden ser destino, más `+ Nueva instalación`.
3. **Clic en una instalación de la lista:** **comienza inmediatamente** la actualización de esa instalación; no se vuelve a pulsar instalar.
4. **Clic en `+ Nueva instalación`:** abre el selector de carpeta; confirmar la carpeta, existente o recién creada, **inicia inmediatamente** la instalación.
5. Ignorar o cancelar el menú no altera archivos.

El usuario puede elegir conscientemente actualizar una instalación distinta de la sugerida. Los nombres de archivos versionados (`Firefox_v02.zip`, etc.) orientan la propuesta, pero no son evidencia suficiente de identidad.

### 19.2 Instalador PAF frente a comprimidos

- **PAF:** ejecutar su **instalador oficial** sobre la instalación elegida, respetando el lanzador portable y sus reglas de actualización. Su lanzador oficial es el ejecutable predeterminado.
- **ZIP/7z/RAR/TAR:** extraer primero en una carpeta de trabajo `App/.temp/<id-operación>/` —por ejemplo, para FirefoxPortable—, y solo después copiar/sobrescribir el destino elegido. La extracción incompleta **nunca** debe convertirse en una instalación parcial.
- Para los ZIP no se implementa en Lite un clasificador sofisticado de archivos de perfil. Se respeta la estructura del paquete y se conserva el perfil externo administrado por PM. Un ZIP que contenga un archivo de configuración en el propio directorio del programa **podría sobrescribirlo**: no prometer conservación universal de contenidos mezclados.
- El color del **icono de instalación** es **verde** si puede iniciarse la operación y **rojo** si, por ejemplo, la aplicación destino está activa o sus archivos no pueden actualizarse. El color comunica disponibilidad, **no seguridad**. Una aplicación activa no se cierra automáticamente para actualizar.
- Se admite detección de errores y eliminación/reversión de elementos creados por una instalación fallida, dentro de lo técnicamente alcanzable; no introducir diagnósticos ni asistentes. La barra de instalación empieza verde y cambia a roja mientras se revierte un error.

### 19.3 Ejecutable principal y criterio de selección

- En PAF se utiliza el **lanzador PortableApps**, no el EXE interno de `App/`.
- En un ZIP con **un único EXE en la raíz**, se registra automáticamente como ejecutable principal.
- En un ZIP con **varios EXE en la raíz**, PM ofrece selección de ejecutable principal; la elección queda en el INI y puede editarse después.
- El usuario decide si el paquete es portable; PM no certifica ni diagnostica su funcionamiento.
- **Solo archivos `.exe` se registran como aplicaciones en Lite.** BAT/CMD/PS1/PY podrán aparecer como archivos, pero no se registran como ejecutables principales de aplicación. Los comandos configurables por INI son una categoría distinta y la interfaz visual para crearlos queda para futuras versiones.

## 20. Una sola carpeta App: aplicaciones instaladas y carpetas manuales

Estructura física simplificada:

```text
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

**No existen carpetas físicas separadas para aplicaciones registradas y pendientes.** PMD interpreta el registro:

- Carpeta de `App/` **registrada** → se muestra su **icono de aplicación** directamente en PMD; no se muestra como carpeta.
- Carpeta de `App/` **no registrada** → se muestra como carpeta; clic sencillo entra y permite explorar.
- Dentro de una carpeta no registrada: **clic sencillo en un EXE = probar/ejecutar sin registrar**; **doble clic en un EXE = registrar y ejecutar**. El usuario puede decidir libremente si registra una carpeta.
- `Other/` se muestra como carpeta normal de PMD y se explora/abre mediante el comportamiento ya definido en v0.1.
- Una vez registrada, la app se muestra como icono y **clic sencillo la ejecuta**.
- PM siempre invoca el **lanzador registrado**. El propio programa decide si reutiliza una ventana, abre instancias múltiples o las bloquea; PM no impone reglas de instancia.

### 20.1 Nombre predeterminado y búsqueda

El nombre por defecto procede del EXE elegido **sin `.exe`**: `MiPrograma64.exe` → `MiPrograma64`. **Ese nombre siempre sirve para buscar y ejecutar** (MagicWord); no puede quedar sin identidad de búsqueda. Los alias son **adicionales, opcionales y editables** en el INI; no sustituyen la búsqueda por nombre.

```ini
[App.MiPrograma]
Name=MiPrograma64
Executable=App/MiPrograma/MiPrograma64.exe
Aliases=
```

Si una carpeta registrada desaparece manualmente, **el icono permanece gris**, sin poder ejecutarse. Desde su menú contextual, **Desinstalar** ejecuta la limpieza disponible y retira la información asociada. Si reaparece antes de desinstalar, puede recuperarse su identidad.

## 21. Estados visuales: tres conceptos distintos

### 21.1 Punto del icono de aplicación (estado de privacidad)

| Punto | Significado |
|---|---|
| **Blanco/gris** | **Sin verificar**; son el mismo estado gráfico, no dos categorías. |
| **Verde** | La aplicación, al cerrarse, **no deja residuos observados** que requieran limpieza adicional de PM. |
| **Amarillo** | Necesita la **limpieza estándar de PM** y, después de ella, el host queda restaurado respecto de los cambios controlados. |
| **Rojo** | **Persisten residuos** tras la limpieza aplicable de PM; se anotan en el log del USB cuando esté disponible. |

La categoría se determina por comprobación, **no por ser PAF o ZIP**. Un PAF no es automáticamente verde; su propio lanzador puede dejar residuos observados. PM no repite sobre PAF la limpieza que le corresponde al lanzador.

### 21.2 Icono contextual de instalación

- **Verde:** operación disponible.
- **Rojo:** operación no disponible ahora, por ejemplo porque el destino está ejecutándose.

No confundir este icono con el punto de privacidad de la aplicación.

### 21.3 LED y barra de la píldora (estado de PM)

| Señal | Significado |
|---|---|
| **LED habitual de reposo** | PM está activo, sin que el LED exprese el nivel de privacidad de una app particular. |
| **Barra blanca** | **Primera ejecución/aprendizaje** de un ZIP desconocido. Avance solo cuando una fase sea realmente medible; no inventar porcentajes mientras el usuario mantiene la app abierta. |
| **Barra verde** | **Instalación/actualización** y **cierre normal de PM**, con progreso medido donde sea posible. |
| **Barra roja tras empezar verde** | **Error de instalación**; permanece roja mientras se eliminan/revierten residuos controlados de la instalación. |
| **Barra roja** | **Cierre forzado** solicitado por el usuario. |
| **LED rojo** | Pérdida del **USB o fuente de datos**; dispara cierre de emergencia. |

El LED global y la clasificación de un icono de app son independientes. Contraer PMD no inicia el cierre ni muestra barra de cierre.

## 22. Motor de portabilidad para aplicaciones ZIP

### 22.1 Responsabilidades

- **Aplicaciones PortableApps PAF:** confiar su ciclo de portabilidad/restauración al **lanzador oficial**. PM puede observar procesos y comprobar resultados de forma **no destructiva**, sin duplicar las recetas del lanzador.
- **Aplicaciones importadas desde comprimidos:** PM descubre, aprende, mueve y restaura los recursos locales mediante recetas propias.
- El observador no se extiende a todo el disco, no requiere servicios persistentes, drivers, elevación ni IA.

### 22.2 Ciclo acordado: renombrar → preparar → ejecutar → guardar → restaurar

Para **ubicaciones previamente aprendidas** en el anfitrión:

1. **Renombrar/resguardar:** si existen datos locales del anfitrión, renombrarlos temporalmente para preservarlos. Dejar constancia de su ubicación original y temporal.
2. **Preparar:** mover/copiar a esas rutas los datos portables conservados en el USB, o crear directorios nuevos si no había datos.
3. **Ejecutar:** lanzar el EXE registrado y supervisar hasta que terminen los procesos que pertenezcan a la aplicación; el lanzador decide sobre instancias múltiples.
4. **Guardar:** mover los datos producidos/modificados durante la ejecución de vuelta al área `Data/` del USB.
5. **Restaurar:** eliminar las rutas temporales creadas para la app y devolver al anfitrión sus carpetas/valores originales renombrados o respaldados. La limpieza pertenece al ciclo de cierre de la aplicación, **incluso cuando se cierra desde su propia X**.

Para claves de registro se utiliza un equivalente de exportación/restauración dirigida; no se «renombra» una clave de Windows como si fuera una carpeta. Las operaciones pendientes se rastrean en el journal del USB.

**Datos portables que se conservan:** tanto `%APPDATA%` como `%LOCALAPPDATA%` de la aplicación, y ajustes asociados del registro que puedan identificarse, sin intentar separar automáticamente cachés y preferencias. El INI podrá excluir carpetas concretas después. Los temporales redirigidos se descartan según las recetas.

### 22.3 Primera ejecución: aprendizaje sin IA

- La primera apertura de un ZIP desconocido puede ser una **ejecución de prueba normal**. PM obtiene el inventario inicial y observa mientras el usuario usa el programa; **no lo cierra automáticamente**.
- Al cerrarse, PM compara el estado anterior/posterior en las ubicaciones acotadas: `%APPDATA%`, `%LOCALAPPDATA%`, `%TEMP%`/`%TMP%`, `HKCU\Software` y rutas conocidas pertinentes. Considera nombres de ejecutable/aplicación como **pistas**, sin limitarse a ellos.
- El inventario previo/posterior, el seguimiento dirigido y las recetas **son complementarios**, no opciones excluyentes. No es necesario vigilar todo Windows en tiempo real.
- PM guarda en el USB los datos nuevos atribuibles a la aplicación, limpia lo que pueda verificar y **aprende las rutas** para el ciclo completo de las siguientes ejecuciones.
- **Limitación aceptada:** si en la primera ejecución una app modifica contenido preexistente que PM no respaldó antes, un snapshot posterior no permite restaurarlo fielmente; no se borra a ciegas. El aprendizaje no equivale a identificación perfecta del proceso autor de cada cambio.
- La barra blanca de la píldora señala la fase de aprendizaje. Si durante la app abierta no puede medirse una fracción real, no se presenta un porcentaje fingido. El procesamiento final muestra progreso real cuando existe una unidad de trabajo contable.

### 22.4 Repeticiones, procesos y conflictos

- Aunque el usuario invoque una app diez veces, PM mantiene **una identidad y receta por aplicación**, no diez perfiles.
- Antes de restaurar recursos compartidos, esperar que terminen los procesos identificados que todavía los utilizan. No alterar decisiones del lanzador sobre múltiples ventanas o instancias.
- Si dos apps gestionadas comparten una ubicación protegida, **no restaurarla mientras la otra la necesita**. La coordinación ha de limitarse a recursos identificados; las colisiones no observadas siguen siendo una restricción técnica.
- Las modificaciones de otras aplicaciones del anfitrión no deben atribuirse automáticamente a PM solo por coincidir en el tiempo.

## 23. Cierre y limpieza

### 23.1 Cierre ordinario de una aplicación

- Al cerrar desde PMD o **desde la propia X de la aplicación**, PM detecta que finalizaron sus procesos y ejecuta el cierre/limpieza que corresponda.
- **Verde:** no necesita limpieza adicional de PM.
- **Amarillo:** PM ejecuta limpieza estándar y restaura las ubicaciones que controla; debe terminar sin residuos observados pendientes.
- **Rojo:** PM ejecuta limpieza normal y conserva en el log del USB los residuos que permanezcan.
- La limpieza es **híbrida**: tras terminar la aplicación se limpian sus recursos relevantes; al cerrar PM se revisan únicamente operaciones pendientes. No repetir un trabajo que ya se completó.
- **Cerrar PMD solo oculta la interfaz**, no termina procesos ni ejecuta limpieza global.

### 23.2 Cierre forzado de aplicación

- El **cierre forzado individual** se invoca **solo desde el menú contextual de una aplicación roja en PMD**.
- Forzado significa **terminar procesos resistentes** y retirar de manera más agresiva los elementos **atribuidos como creados durante la sesión**, además de restaurar recursos originales respaldados.
- **No** significa eliminar indiscriminadamente claves antiguas o archivos compartidos del anfitrión. Debe respetar la propiedad y preexistencia de datos; algunas operaciones pueden no ser posibles sin permisos elevados.
- Cerrar normalmente una app mediante su propia X **siempre** implica cierre normal; no inicia forzado de manera implícita.

### 23.3 Cierre normal/rápido/crítico de PM

Se conservan las opciones de cierre global desde la píldora definidas en v0.1. El cierre **normal** utiliza barra verde; las acciones de cierre **forzado** utilizan barra roja. Los tiempos, diferencias exactas entre rápido y crítico y medidas de avance externo permanecen sujetos a diseño técnico. Ninguno se activa por contraer PMD.

### 23.4 Desconexión del USB o de la fuente de datos

- Ante pérdida de fuente, **LED rojo y cierre de emergencia automático**, sin diálogo ni espera de reconexión.
- PM impide nuevos lanzamientos, fuerza el cierre de los procesos bajo su responsabilidad e intenta limpiar y restaurar inmediatamente todo lo posible.
- **No guarda logs, respaldos de emergencia ni informes persistentes en el anfitrión**, porque el objetivo es minimizar su huella y el USB ya no está.
- Cuando la fuente reaparece, una **nueva ejecución** parte de la **última configuración que realmente quedó guardada en el USB**; los cambios no guardados pueden perderse.
- Una estructura mínima de **recuperación residente en RAM** puede conservar durante la sesión las recetas, referencias a renombramientos y datos pequeños necesarios para actuar tras la desconexión. Los archivos originales renombrados permanecen temporalmente en el anfitrión hasta su restauración; no es imprescindible mantener todo su contenido en RAM.
- Es un **objetivo sujeto a prueba técnica** que el módulo de cierre siga operativo cuando desaparecen los binarios del USB. RAM ordinaria no garantiza exclusión del archivo de paginación. El cierre de emergencia reduce residuos, pero no puede garantizar resultados si Windows termina PM abruptamente, la memoria se pierde o faltan permisos.
- **No convertir errores en informes al usuario** cuando PM ya no puede presentar nada. Sin fuente de datos, el flujo final es: cerrar, limpiar cuanto sea posible y terminar.

## 24. Operaciones del menú contextual: Restablecer, Archivar, Desinstalar

**El flujo habitual de instalación sirve también para actualizar o reinstalar.** No añadir acciones separadas «Reparar» o «Actualizar» al menú contextual.

| Acción | Programa en `App/` | Datos portables en USB | Registro/receta PM | Durante ejecución |
|---|---|---|---|---|
| **Restablecer** | Se conserva | **Se eliminan** | **Se conserva** | **Deshabilitada** |
| **Archivar** | **Se elimina** | **Se conservan** | **Se conserva** | **Deshabilitada** |
| **Desinstalar** | **Se elimina** | **Se eliminan** | Se retira del registro activo tras limpieza | Solicita cerrar, limpia y desinstala |

### 24.1 Restablecer

- Solo cuando la aplicación **no está en uso**; la opción aparece en gris cuando está ejecutándose.
- Elimina los datos **portables de la aplicación en el USB**, no el ejecutable ni la receta de limpieza aprendida. Conserva nombre, identificador y alias.
- No borra datos automáticamente por fallos, ni diagnostica errores.
- Como la acción elimina datos personales, puede requerir una confirmación mínima; el detalle exacto de confirmación no ha sido cerrado.

### 24.2 Archivar

- Solo cuando la aplicación está cerrada.
- Elimina sus **binarios** para ahorrar espacio, conserva **datos portables, identidad, alias y receta**.
- El icono **desaparece de PMD** para ahorrar espacio visual.
- Sigue disponible en la **búsqueda de la píldora** como `Nombre (archivado)`, pero **no es ejecutable**.
- Seleccionarla desde la búsqueda **abre el selector del paquete ZIP o PAF** y reutiliza el flujo de instalación. Al reinstalar, PM recupera los datos conservados.
- PAF: como `Data/` suele estar dentro de `App/<AppPortable>/`, **mover esa `Data/` a un área de archivo del USB** antes de retirar binarios, y devolverla al reinstalar. ZIP: conservar perfiles ya separados bajo `Data/` de PM. El destino físico final de archivos PAF se fijará al implementar.

### 24.3 Desinstalar

- Ejecuta cierre normal de los procesos que pertenezcan a la app, limpieza de residuos del anfitrión y eliminación de archivos/programa/datos/registro, dentro de los límites de acceso.
- La misma acción aplica a iconos **grises por carpeta faltante**: aún debe ejecutar la receta o las restauraciones pendientes antes de retirar la asociación, sin pretender borrar una carpeta que ya no existe.
- No confundir un icono gris por **carpeta faltante** con una aplicación **archivada**: las archivadas no se muestran en PMD y sí se distinguen por `(archivado)` en búsqueda.
- No eliminar recetas o referencias imprescindibles antes de intentar resolver limpiezas pendientes. Si se requiere conservar un registro técnico residual, debe residir en el USB, no en el anfitrión.

### 24.4 Receta independiente de los datos

La receta es **configuración de PM**, no datos de usuario de la aplicación. Por eso **Restablecer no la borra**; Archivar también la conserva. Las exclusiones de carpetas y las rutas aprendidas pueden editarse en el INI.

## 25. Configuración y almacenamiento sugeridos (conceptuales)

La estructura física de v0.1 mantiene **una sola carpeta `App/`**. La propuesta siguiente describe responsabilidades; no obliga todavía a nombres de archivos exactos:

```text
<USB>/
├── pm.exe
├── App/
│   ├── .temp/                       # Extracción previa a instalar
│   ├── FirefoxPortable/             # PAF, conserva App/Data/Other propios
│   ├── MiPrograma/                  # ZIP registrado o carpeta manual
│   └── OtraCarpeta/                 # Sin registrar, visible como carpeta en PMD
├── Data/
│   ├── apps.ini                     # Nombres predeterminados, alias, identidad, estado
│   ├── config.ini                   # Ajustes generales
│   ├── recipes/                     # Recetas y rutas aprendidas de PM
│   ├── apps/                        # Datos portables de ZIP aprendidos
│   │   └── id-estable/
│   │       ├── roaming/
│   │       ├── local/
│   │       └── registry/
│   ├── archived/                    # Datos preservados de PAF archivados
│   ├── logs/                        # Incidencias con USB disponible
│   └── session.journal              # Operaciones pendientes, solo USB
└── Other/                           # Carpeta normal de documentos
```

Ejemplo conceptual de receta, no formato normativo definitivo:

```ini
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

El ID interno debe permanecer estable aunque cambien el nombre visible y los alias. PM podrá identificar una carpeta físicamente presente pero no registrada, y un registro cuyo directorio fue retirado manualmente.

## 26. Funciones aplazadas expresamente

- **Editor de texto integrado**, similar a Bloc de notas, para visualizar/editar BAT, CMD, PS1, PY y otros archivos de texto sin depender de software del anfitrión.
- **Gestión visual de comandos personalizados**, inspirada funcionalmente en el `Add` de SlickRun, pero con identidad de PM; **por ahora se gestionan mediante INI**. Los comandos no equivalen automáticamente a aplicaciones registradas en PMD.
- **Restablecimiento parcial:** borrar únicamente preferencias conservando ciertos perfiles/documentos; Lite solo dispone de **Restablecer datos completo**.
- PM Secure y PM Cloud siguen en la hoja de ruta de v0.1, fuera del alcance de Lite.
- Descubrimiento/atribución de cambios más avanzado, controladores, sandbox, aislamiento integral de procesos y analítica profunda quedan fuera de la primera edición.

## 27. Pendientes reales antes de implementar

Quedan decisiones técnicas acotadas; no requieren reabrir los principios funcionales anteriores:

1. **Prueba de concepto RAM y desconexión:** comprobar si PM puede ejecutar cierre y restauración con el dispositivo desconectado y sin binarios accesibles. Identificar las funciones que deben residir en memoria.
2. **Observación inicial realista:** costo de enumeración y snapshots de AppData/LocalAppData/registro; cómo manejar escrituras intercaladas y contenido preexistente sin respaldo sin borrado indiscriminado.
3. **Conflictos de rutas entre aplicaciones:** política concreta de coordinación, especialmente cuando un PAF oficial y un ZIP acceden al mismo recurso; no sobrescribir el trabajo de un lanzador oficial.
4. **Formato y ubicación final de metadatos:** archivos INI/recetas/journal, identificación estable, exclusiones y migración de datos de PAF archivados.
5. **Progreso medible:** límites de porcentaje real cuando un instalador PAF no expone progreso o cuando el aprendizaje depende del cierre decidido por el usuario.
6. **Búsqueda:** prioridad de coincidencias y comportamiento de Enter sobre resultados `(archivado)`; el selector de paquete es la decisión funcional, pero faltan los detalles de navegación.
7. **Restricciones del usuario estándar:** pruebas de permisos, restauración del registro y comportamiento frente a procesos que no responden.
8. **Código y UI existentes:** antes de proponer crear módulos o interfaces, comprobar si ya existen en el proyecto. El proyecto sigue en **fase de diseño**.

## 28. Correcciones explícitas respecto de v0.1

- Donde v0.1 decía que PMD **nunca muestra carpetas de `App/`**, reemplazar por: **muestra las carpetas no registradas de `App/`; las registradas se representan como iconos**.
- Donde v0.1 prometía que PM «declara» residuos o reporta errores al usuario, reemplazar por: **limpieza automática y discreta; log solo en USB cuando esté disponible y sea útil; no crear reportes en el host**.
- Donde v0.1 sugería varios mecanismos de limpieza para PAF, reemplazar por: **la limpieza corresponde al lanzador oficial; PM puede verificar sin duplicar acciones destructivas**.
- Añadir **Restablecer** y **Archivar**, además de Desinstalar; **sin** botón independiente de Reparar/Actualizar.
- Clasificación de privacidad **gris/blanco, verde, amarillo, rojo** según el resultado de limpieza, separada de LED/barra de píldora e icono de instalación.
- Sustituir la política de cierre de app: **cierre natural siempre normal; cierre forzado individual únicamente desde el menú contextual de una app roja en PMD**.
- Para pérdida del USB: **cierre de emergencia automático** con apoyo en estado en RAM; no esperar reconexión ni dejar registros persistentes en el host.

---

**Resumen de alcance:** terminada la definición funcional de instalación, registro, ejecución, aprendizaje básico, portabilidad ZIP, clasificación de privacidad, cierres, restablecimiento, archivado y desinstalación. Pendiente la validación técnica y algunos comportamientos de detalle, no una nueva capa de administración o diagnóstico.
