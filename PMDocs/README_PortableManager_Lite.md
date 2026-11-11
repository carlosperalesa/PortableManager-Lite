# PortableManager Lite

**Estado del proyecto:** diseño funcional consolidado y arquitectura
técnica en validación.\
**Plataforma objetivo:** Windows, ejecutado desde un dispositivo USB.\
**Edición:** Lite.

## 1. Idea principal

PortableManager Lite (PM) es un entorno portátil, discreto y de
intervención mínima que permite ejecutar aplicaciones y abrir documentos
desde un USB, conservando allí la configuración y los datos portables
siempre que la aplicación lo permita.

PM también prepara, registra y revierte los cambios temporales que él
mismo realiza en el equipo anfitrión. Su objetivo es reducir al mínimo
los rastros de información personal en Windows, especialmente en equipos
compartidos.

PM no es un explorador de archivos tradicional, un administrador general
de Windows ni un virtualizador universal. La interacción principal es
una píldora compacta que abre búsqueda y un escritorio temporal
(PM-Desktop o PMD) con iconos y contenido del USB.

## 2. Filosofía y principios

1.  **Portabilidad primero:** preferir las opciones nativas de cada
    aplicación para guardar datos directamente en el USB.
2.  **Intervención mínima:** no instalar servicios, drivers ni
    componentes persistentes en Windows; no exigir privilegios de
    administrador para el flujo ordinario.
3.  **Restaurar, no invadir:** respaldar el estado previo del anfitrión
    y restaurar solo las rutas y claves que PM haya preparado y
    registrado.
4.  **No borrar por suposición:** las rutas compartidas, los cambios
    ambiguos y los datos ajenos no se eliminan por inferencia.
5.  **PAF conserva su responsabilidad:** los lanzadores oficiales de
    PortableApps administran su propia portabilidad y limpieza; PM no
    duplica sus recetas.
6.  **PM es el propietario:** PM controla la sesión y su cierre. PMGuard
    (PMG) es una sombra de emergencia prescindible.
7.  **Simplicidad verificable:** preferir recetas pequeñas, reglas
    explícitas y pruebas reproducibles antes que virtualización o
    automatización compleja.
8.  **Honestidad sobre los límites:** PM reduce rastros verificables,
    pero no promete cero residuos en todos los equipos Windows.

## 3. Cómo se organiza

``` text
<USB>\
├── pm.exe
├── App\                  # Aplicaciones y paquetes
├── Data\
│   ├── config.ini        # Configuración general
│   ├── apps.ini          # Identidad, alias, asociaciones y estado
│   ├── recipes\          # Recetas permanentes por aplicación
│   ├── apps\             # Datos portables persistentes de ZIP
│   ├── archived\         # Datos de PAF archivadas
│   ├── logs\             # Incidencias/residuos, solo en USB
│   └── session.journal   # Transacciones de instalación/actualización
└── Other\                # Documentos y carpetas del usuario
```

Durante una sesión activa existe además un manifiesto transitorio en
`%TEMP%`:

``` text
%TEMP%\PortableManager\Session\<SessionID>\session.ini
```

No confundir:

-   **Receta permanente:** describe cómo tratar una aplicación y
    permanece en el USB.
-   **Manifiesto TEMP:** contiene las operaciones concretas de la sesión
    y los pasos inversos para restaurar el anfitrión. PM lo escribe y lo
    lee; PMG lee el mismo archivo si PM desaparece.

## 4. Alcance funcional de Lite

### Interacción y escritorio

-   Píldora compacta con LED y búsqueda por atajo.
-   Autocompletado por prefijo, MagicWords, alias y `_DEFAULT`.
-   PM-Desktop temporal con iconos de aplicaciones registradas y
    contenido de `Other/`.
-   Contraer PMD oculta la interfaz; no cierra PM ni las aplicaciones.

### Aplicaciones y archivos

-   Gestión de aplicaciones portables en `App/`.
-   Ejecución del lanzador oficial cuando existe.
-   Instalación/actualización desde paquetes PAF y archivos ZIP, 7z, RAR
    y TAR, según las reglas de cada formato.
-   Asociaciones de extensiones con aplicaciones portables.
-   Apertura con aplicaciones del anfitrión solo con consentimiento
    explícito.

### Portabilidad y limpieza

-   PAF: el lanzador oficial gestiona su propia portabilidad.
-   ZIP: PM aprende rutas y claves candidatas, conserva datos portables
    en el USB y restaura los cambios controlados del anfitrión.
-   Prioridad ZIP: (1) mecanismo nativo de la aplicación; (2) variables
    de entorno/adaptadores específicos del proceso; (3) preparación
    temporal de rutas del anfitrión con `.pmbkp`, solo como caso borde.
-   Las carpetas compartidas no se renombrarán completas.
-   Las claves de registro se tratan mediante snapshots y restauración
    dirigida, no como si fueran carpetas.

### Ciclo de vida

-   Cierre normal, rápido y crítico.
-   Cierre crítico prioritario: no espera a PAF ni a aplicaciones ZIP;
    termina los procesos y restaura el anfitrión con el manifiesto
    local. Los datos de aplicación todavía no guardados en el USB pueden
    perderse.
-   Restablecer, Archivar y Desinstalar según las reglas de v0.2.1.

## 5. PM y PMGuard

PM es el proceso principal y dueño de la sesión.

PM escribe antes de cada modificación del anfitrión un manifiesto
operativo en `%TEMP%`. PM utiliza ese manifiesto en su propio cierre.
PMGuard es una copia de sombra que puede leer exactamente el mismo
manifiesto si PM termina inesperadamente.

-   PMG no administra aplicaciones ni tiene recetas independientes.
-   Si PMG desaparece mientras PM sigue vivo, PM continúa y conserva la
    responsabilidad de limpiar/restaurar. Puede reiniciar PMG, pero no
    debe cerrar la sesión solo porque PMG terminó.
-   Si PM desaparece, PMG intenta ejecutar la rutina de emergencia.
-   Una exclusión mutua evita que PM y PMG restauren simultáneamente.
-   El runtime local y la recuperación tras desconexión USB son
    objetivos que deben validarse mediante pruebas de concepto.

## 6. Alcances y limitaciones

PM Lite:

-   Está pensado para Windows y ejecución desde USB.
-   No necesita instalar un servicio o driver.
-   No promete cero rastros ni controla todos los subsistemas de
    Windows.
-   No puede atribuir con certeza absoluta cada escritura de terceros.
-   No modifica indiscriminadamente HKLM ni claves globales.
-   No mueve carpetas compartidas ni borra cambios ambiguos.
-   No garantiza que toda aplicación respete `APPDATA`, `LOCALAPPDATA`,
    `TEMP` o `TMP`.
-   No garantiza guardar datos de una aplicación durante un cierre
    crítico.
-   No garantiza restauración ante corte eléctrico, apagado abrupto,
    desaparición simultánea de PM/PMG, falta de permisos o pérdida del
    manifiesto.
-   Puede dejar residuos de PAF si el cierre crítico interrumpe el
    lanzador antes de que complete su propia rutina.
-   No es un sandbox, un antivirus, un sistema de borrado seguro ni una
    herramienta de anonimato.

Los estados blanco/gris, verde, amarillo y rojo describen el resultado
dentro del alcance observado; no son una certificación absoluta de
privacidad.

## 7. Estado actual del proyecto

**Fase actual: diseño y validación técnica previa a implementación.**

Consolidado en los documentos:

-   Filosofía y alcance de Lite.
-   Píldora, búsqueda, MagicWords y PM-Desktop.
-   Instalación, actualización, asociaciones y gestión de aplicaciones.
-   Diferencias entre PAF y aplicaciones ZIP.
-   Aprendizaje inicial sin IA.
-   Prioridades de portabilización y uso de `.pmbkp`.
-   Registro dirigido y restauración del anfitrión.
-   Cierre normal, rápido y crítico.
-   Modelo PM/PMG y manifiesto temporal.

**No se debe interpretar que estas funciones ya estén implementadas.**
Los documentos describen decisiones y arquitectura. Existen pruebas
exploratorias previas de algunas aplicaciones y escenarios, pero no
equivalen a una versión funcional completa.

### Pruebas de concepto pendientes prioritarias

1.  Desconexión del USB con runtime PM/PMG preparado localmente.
2.  Manifiesto TEMP: escritura segura, recuperación y exclusión mutua.
3.  Job Objects compartidos entre PM y PMG y límites de captura de
    procesos.
4.  Renombrado/restauración `.pmbkp`, carpetas compartidas, colisiones y
    fallos intermedios.
5.  Captura/restauración de HKCU y conflictos concurrentes.
6.  Aprendizaje de ZIP, atribución de rutas y conservación de datos en
    el USB.
7.  Diferencias reales entre cierre normal PAF y cierre crítico.
8.  Compatibilidad y permisos en cuentas estándar de distintas versiones
    de Windows.

## 8. Ideas futuras --- fuera de Lite

Estas ideas no forman parte de la implementación actual y no deben
adelantarse sin cerrar Lite:

-   **PM Secure:** estudiar protección de datos mediante BitLocker To Go
    u otra estrategia cifrada compatible con las restricciones del
    equipo anfitrión.
-   **PM Cloud:** estudiar sincronización y caché con servicios como
    OneDrive/rclone, sin prometer ejecución universal directa desde
    WebDAV.
-   **Editor integrado:** visualizar y editar archivos de texto desde
    PM.
-   **Gestión visual de comandos:** interfaz para crear/editar comandos
    personalizados, hoy previstos mediante INI.
-   **Restablecimiento parcial:** permitir borrar preferencias
    seleccionadas conservando otros datos portables.
-   **Atribución avanzada:** mejorar la identificación de escrituras y
    los conflictos entre aplicaciones, sin convertir Lite en un
    virtualizador universal.

## 9. Documentos vigentes y precedencia

1.  **`PortableManager-Lite-Diseno-v0.1-base.md`** --- diseño funcional
    original. Es la base histórica y debe leerse junto con v0.2.1.
2.  **`PortableManager-Lite-Diseno-v0.2.1.md`** --- decisiones
    funcionales posteriores. Prevalece sobre v0.1 cuando existe alguna
    diferencia.
3.  **`ARQUITECTURA_PM_LITE_PM_GUARD-v1.1.md`** --- arquitectura técnica
    del motor y PMG. Desarrolla las decisiones funcionales; no puede
    contradecir v0.2.1.

El README resume el proyecto y no sustituye a los tres documentos de
diseño.

## 10. Reglas para continuar el desarrollo

-   Antes de crear un módulo, interfaz o archivo, revisar si ya existe
    en el proyecto.
-   No duplicar la portabilidad que ya administra el lanzador PAF.
-   No borrar ni mover datos si no se conoce su pertenencia.
-   Mantener la receta permanente en el USB y el manifiesto activo en
    TEMP.
-   PM sigue siendo el propietario de la sesión; PMG es una sombra.
-   El cierre crítico prioriza la restauración del anfitrión sobre la
    rutina normal de las aplicaciones.
-   Toda garantía técnica debe estar respaldada por una prueba
    reproducible.

## 11. Referencias técnicas principales

-   PortableApps Launcher:
    https://portableapps.com/manuals/PortableApps.comLauncher/ref/index.html
-   PortableApps, tratamiento del registro:
    https://portableapps.com/manuals/PortableApps.comLauncher/topics/registry.html
-   PortableApps, archivos y directorios:
    https://portableapps.com/manuals/PortableApps.comLauncher/ref/launcher.ini/filesystem.html
-   Microsoft, bloques de entorno:
    https://learn.microsoft.com/en-us/windows/win32/procthread/environment-variables
-   Microsoft, `RegNotifyChangeKeyValue`:
    https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regnotifychangekeyvalue
-   Microsoft, `ReadDirectoryChangesW`:
    https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw
-   Microsoft, Job Objects:
    https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects
