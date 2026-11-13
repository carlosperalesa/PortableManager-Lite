# PortableManager Lite — Continuación del diseño funcional

**Versión:** 0.2.2 (Consolidación normativa tras validación técnica de PoCs)\
**Fecha:** 11 de noviembre de 2026\
**Estado:** diseño normativo consolidado con validación empírica aprobada.\
**Documentos base:** `PortableManager-Lite-Diseno-v0.1-base.md` y `PortableManager-Lite-Diseno-v0.2.1.md`.\
**Evidencias técnicas:** [AUDITORIA_CONTINUIDAD.md](../AUDITORIA_CONTINUIDAD.md), [PLAN_POC_PM_LITE.md](../PLAN_POC_PM_LITE.md), [RESULTADOS_POC_PM_LITE.md](../RESULTADOS_POC_PM_LITE.md), [RESULTADOS_APLICACIONES_REALES.md](../RESULTADOS_APLICACIONES_REALES.md) y [CONTINUIDAD_DESARROLLO.md](../CONTINUIDAD_DESARROLLO.md).

> **Regla de interpretación:** este documento **complementa y sustituye** las decisiones previas de v0.1 y v0.2.1 donde existan discrepancias o precisiones técnicas derivadas de las PoCs. PortableManager Lite (PML) distribuye `pml.exe`. PortableManager Desktop (PMD) abarca toda la presentación gráfica dentro de PML, incluida la píldora, el escritorio semitransparente, los menús y las ventanas. Contraer el escritorio mantiene PML en ejecución.

---

## 18. Principio de producto y alcance

- PML Lite es un entorno portátil, pequeño, discreto y de intervención mínima. Su propósito es permitir usar aplicaciones y documentos del USB conservando sus datos allí y **reduciendo al mínimo los rastros de información personal en Windows anfitrión**, particularmente en entornos compartidos como cibercafés.
- PML **no** es un sistema de diagnóstico, auditoría ni administración general de Windows. No presenta asistentes, informes ni advertencias por cada incidencia. El usuario decide si restablece, reinstala o elimina una aplicación; PML no borra automáticamente sus **datos portables** porque una aplicación falle.
- Se aprovechan comportamientos ya resueltos por PortableApps.com. Los lanzadores **PAF oficiales administran su propia portabilidad y limpieza**; PML no les superpone un segundo motor de restauración. PML gestiona la portabilidad de aplicaciones instaladas desde ZIP/7z/RAR/TAR.
- La obligación de limpiar **residuos del anfitrión** al terminar una aplicación es distinta de borrar deliberadamente los **datos personales guardados en el USB**. La primera es automática según el ciclo de vida; la segunda únicamente ocurre cuando el usuario ordena Restablecer o Desinstalar.
- No añadir IA, cifrado, nube, controladores o diagnósticos complejos a Lite. No garantizar ausencia absoluta de trazas en todos los hosts: sin privilegios elevados no se pueden identificar o revertir todas las escrituras de terceros.

---

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
- **ZIP/7z/RAR/TAR:** extraer primero en una carpeta de trabajo `App/.temp/<id-operación>/` y solo después copiar/sobrescribir el destino elegido. La extracción incompleta **nunca** debe convertirse en una instalación parcial.
- Para los ZIP no se implementa en Lite un clasificador sofisticado de archivos de perfil. Se respeta la estructura del paquete y se conserva el perfil externo administrado por PML. Un ZIP que contenga un archivo de configuración en el propio directorio del programa **podría sobrescribirlo**: no prometer conservación universal de contenidos mezclados.
- El color del **icono de instalación** es **verde** si puede iniciarse la operación y **rojo** si, por ejemplo, la aplicación destino está activa o sus archivos no pueden actualizarse. El color comunica disponibilidad, **no seguridad**. Una aplicación activa no se cierra automáticamente para actualizar.
- Se admite detección de errores y reversión de elementos creados por una instalación fallida; no introducir diagnósticos ni asistentes. La barra de instalación empieza verde y cambia a roja mientras se revierte un error.

### 19.3 Ejecutable principal y criterio de selección

- En PAF se utiliza el **lanzador PortableApps**, no el EXE interno de `App/`.
- En un ZIP con **un único EXE en la raíz**, se registra automáticamente como ejecutable principal.
- En un ZIP con **varios EXE en la raíz**, PML ofrece selección de ejecutable principal; la elección queda en el INI y puede editarse después.
- El usuario decide si el paquete es portable; PML no certifica ni audita la seguridad o portabilidad absoluta de binarios desconocidos.

---

## 20. Clasificación de aplicaciones por colores

- **Blanco/gris:** aplicación sin verificar o recién añadida sin ejecución previa.
- **Verde:** aplicación validada empíricamente que no deja residuos personales en el anfitrión (mecanismo nativo portable, Prioridad 1). Ejemplo: `SumatraPDF`.
- **Amarillo:** aplicación que escribe en rutas o claves conocidas del anfitrión pero queda completamente restaurada mediante las rutinas estándar de PML (Prioridades 2, 3 o wrappers con reglas de Registro). Ejemplos: `PuTTY` (Registro), `FirefoxPortable` (PAF integrado).
- **Rojo:** aplicación que deja residuos fuera de alcance, utiliza servicios del sistema, controladores, o rutas ambiguas que PML no puede aislar ni restaurar de forma determinista y segura. Se permite su ejecución bajo advertencia explícita al usuario, pero PML no garantiza portabilidad.

---

## 21. Estrategia de Prioridades para Aislamiento de Datos

Para aplicaciones no-PAF (ZIP/directorios), PML evalúa en orden estricto:

1. **Prioridad 1 — Redirección nativa por parámetros o INI del ejecutable:**
   - La aplicación admite flags de línea de comandos (ej. `-profile`, `-portable`) o configuración local en su propia carpeta para almacenar datos directamente en el USB.
2. **Prioridad 2 — Variables de entorno temporales del proceso:**
   - Redirección de variables (`APPDATA`, `LOCALAPPDATA`, `USERPROFILE`) inyectadas exclusivamente en el bloque de entorno de `CreateProcessW` para el proceso hijo y su árbol de Job Object.
3. **Prioridad 3 — Directory Junctions (`IO_REPARSE_TAG_MOUNT_POINT`, `0xA0000003`):**
   - **Validación empírica aprobada:** se utiliza exclusivamente cuando la ruta destino en el anfitrión es privativa de la aplicación (ej. `%APPDATA%\Vendor\AppName`).
   - Requiere NTFS en el anfitrión (`C:\Users\...`), soportando cualquier sistema de archivos en el USB (`E:\`, NTFS, FAT32 o exFAT).
   - Secuencia obligatoria:
     1. Si la carpeta del anfitrión existe, renombrar a `<carpeta>.pmbkp`. Si no existía, registrar `original_existed = false`.
     2. Crear Directory Junction apuntando a la carpeta de datos del USB (`Data/apps/<id-estable>/...`).
     3. Ejecutar aplicación bajo Job Object.
     4. Al cerrar: invocar `RemoveDirectoryW` sobre la Junction (no borra los datos destino en el USB ni sigue el enlace).
     5. Si existía respaldo `.pmbkp`, restaurarlo a su nombre original. Si no existía, el anfitrión queda limpio.
     6. Si el USB fue desconectado, `RemoveDirectoryW` sobre la Junction huérfana tiene éxito en Windows sin errores.

---

## 22. Aprendizaje Ligero de Aplicaciones (Fase 6; decisión del propietario 2026-11-12)

**Propósito exclusivo.** Identificar los archivos, carpetas y claves de Registro del anfitrión que la aplicación afecta. El wrapper describe los recursos y las operaciones inversas. El aprendizaje deja la configuración y los perfiles bajo control de la aplicación.

1. **Identificación de candidatos:** usar metadatos PE (`CompanyName`, `ProductName` con `GetFileVersionInfoW` / `VerQueryValueW`) para acotar la inspección, por ejemplo, a `%APPDATA%\\<Company>\\<Product>`, `%LOCALAPPDATA%\\<Company>\\<Product>` y `HKCU\\Software\\<Company>\\<Product>`. Los metadatos y las rutas candidatas no demuestran por sí mismos propiedad ni modificación.
2. **Observación y línea base:** antes de ejecutar, registrar existencia y estado de los recursos atribuibles. Identificar mediante observación/diferencias qué recursos fueron efectivamente creados, modificados o eliminados. No considerar un candidato como residuo sin evidencia.
3. **Wrapper de limpieza:** guardar `wrapper.ini` en el directorio real de la instalación indicado por `LearningRequest::app_dir`. Registrar exclusivamente las ubicaciones afectadas, sus tipos, la condición previa y la operación inversa segura; usar variables estándar y referencias relativas cuando se persista un wrapper portable. No guardar rutas absolutas del anfitrión en la configuración persistente.
4. **Respaldo operativo:** si un recurso del anfitrión existía, respaldarlo **temporalmente solo para restaurarlo** (`.pmbkp` para carpetas exclusivas; `.reg` en la sesión para claves HKCU admitidas). Al finalizar, terminar primero los Jobs, retirar los recursos creados por la aplicación y restaurar exactamente los originales. Conservar respaldos si falla la restauración.
5. **Datos de la aplicación:** la configuración portable es responsabilidad de cada aplicación o del mecanismo de portabilidad ya aprobado (modo nativo, variables de entorno del proceso o Junction hacia los datos que la aplicación administra). LearningEngine no copia, cosecha ni archiva configuraciones como parte de generar wrappers.
6. **No interferencia y cierre crítico:** no tocar recursos compartidos, ajenos o de atribución ambigua. En pérdida del USB o caída crítica, omitir toda escritura al dispositivo y restaurar el anfitrión usando el manifiesto y los respaldos operativos accesibles en `%TEMP%`; el cierre nunca debe depender de leer un wrapper en el USB.
7. **Separación de responsabilidades:** el manifiesto temporal de sesión contiene operaciones concretas write-ahead para la recuperación inmediata; los wrappers persistentes son descripciones de recursos observados para limpiezas posteriores. Un wrapper no sustituye el manifiesto ni habilita una limpieza destructiva sin revalidación.

La actualización por copia encima debe preservar el wrapper existente. El diseño de Fase 8 deberá tratar `wrapper.ini` como nombre reservado, detectar la colisión con un archivo del paquete y evitar que el paquete reemplace las reglas aprendidas.

---

## 23. Ciclo de Vida y Protocolo de Cierre

PML implementa tres modos deterministas de finalización:

### 23.1 Cierre Normal (`NORMAL`)
- El usuario solicita salir de PML o de la aplicación.
- Cierre cooperativo mediante `WM_CLOSE` a ventanas de los procesos del Job Object. El motor nativo concede hasta 5 segundos cuando encuentra ventanas notificables.
- No realizar cosecha ni respaldo de configuraciones de aplicaciones; cerrar los Jobs y ejecutar limpieza/restauración con el manifiesto operativo.
- Desmantelamiento de Junctions (`RemoveDirectoryW`).
- Restauración transaccional de claves de Registro desde el respaldo operativo `.reg` local.
- Restauración de carpetas `.pmbkp` del anfitrión.
- Salida limpia de PML y desactivación del monitor PMGuard.

### 23.2 Cierre Rápido (`FAST`)
- El usuario ordena cierre forzado o expulsión inminente.
- Envío de señal de cierre con temporizador estricto (timeout de 200 ms).
- Si los procesos no terminan en el tiempo límite, llamada inmediata a `TerminateJobObject(hJob, 0)`.
- No realizar cosecha de datos; priorizar la terminación del Job y la restauración del anfitrión.
- Restauración prioritaria del anfitrión (Junctions y Registro).

### 23.3 Cierre Crítico de Emergencia (`CRITICAL`)
- Activado por **desconexión física abrupta del USB** o por **caída inesperada del proceso `pml.exe`**.
- El USB ya no es accesible o no es confiable: **se omiten todas las escrituras al USB** para evitar corrupción o bloqueos de E/S.
- Terminación inmediata de procesos huérfanos mediante `TerminateJobObject`.
- PMGuard (ejecutándose desde `%TEMP%`) toma el control mediante la adquisición del Mutex de sesión.
- Eliminación de Junctions huérfanas en el anfitrión.
- Restauración del Registro y carpetas del anfitrión desde los respaldos locales `.reg` y `.pmbkp`, respectivamente.
- **Protocolo de Residuos no Restaurables:** Si un archivo o clave no puede restaurarse (ej. por bloqueo de antivirus):
  - Los respaldos `.pmbkp` y `.reg` necesarios para restaurar se conservan intactos.
  - Se genera el archivo `%TEMP%\pmlog` (texto plano, sin extensión).
  - Se registran exclusivamente las rutas afectadas y los códigos Win32 de error.
  - PML y PMG finalizan sin bloquear el sistema.

El [protocolo nativo de Fase 7](../fase07/README_FASE07.md) describe el drenaje de Jobs, la recuperación de PMGuard y la finalización de temporales.

---

## 24. Consolidación v0.2.2-beta.3 (PMD, Navegación por Árbol UI y LearningEngine)

1. **Árbol explícito de UI (`UiNode`) en PMD**:
   - Jerarquía determinista: `Pill` ↔ `Search` ↔ `ModuleBar [Escritorio | Notas | Config]` ↔ rama activa del módulo (`DesktopGrid`, `NotesTabs` ↔ `NoteEditor`, `ConfigList` ↔ `ConfigFieldEdit`).
   - Navegación espacial coherente: `Down` desciende en la jerarquía, `Up` asciende al nodo padre, `Left`/`Right` recorre hermanos (`ModuleBar`, `NotesTabs`, `DesktopGrid`) o entra/sale de campos (`ConfigList` ↔ `ConfigFieldEdit`), y `Esc` cancela edición o cierra el módulo.
2. **Estabilidad visual de PMD (sin parpadeo)**:
   - Caché de `HICON` en `pmd_graphics.cpp` para evitar extracciones repetidas por frame.
   - Supresión de `WM_ERASEBKGND`/`WM_PAINT` en controles hijos ocultos (`edit_`, `list_`) y temporizador de caret (`kCaretTimerId`) activo únicamente cuando hay edición de texto visible.
   - `EnsureVisibleSelection()` corregido usando `per_row` (`1` en `List`/`Details`, `cols_` en grillas).
3. **Configuración de aplicaciones y menú contextual**:
   - Configuración individual integrada en `Tab::Config` (`Nombre y alias`, `Extensiones`, `Argumentos`, `Navegador predeterminado`) sin botón atrás: se sale con `Esc` o cambiando de pestaña/clic fuera.
   - Opción `Evaluar` en el menú contextual de app deshabilitada mientras existan aplicaciones en ejecución (`!workers_.empty()`).
   - Apertura de un `.exe` dentro de una carpeta `Apps\<Carpeta>` no registrada: registro automático y lanzamiento inmediato vía `LearningEngine`.
4. **LearningEngine y Junctions**:
   - Creación temporal de directorios padre faltantes en `%APPDATA%`/`%LOCALAPPDATA%` antes de `JunctionPrepare`, con limpieza automática si quedan vacíos al finalizar.
   - Registro granular de sub-pasos y códigos Win32 en `Data\logs\learning_<app>.log`.
