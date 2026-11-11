# PortableManager Lite — Continuación del diseño funcional

**Versión:** 0.2.2 (Consolidación normativa tras validación técnica de PoCs)\
**Fecha:** 11 de noviembre de 2026\
**Estado:** diseño normativo consolidado con validación empírica aprobada.\
**Documentos base:** `PortableManager-Lite-Diseno-v0.1-base.md` y `PortableManager-Lite-Diseno-v0.2.1.md`.\
**Evidencias técnicas:** [AUDITORIA_CONTINUIDAD.md](file:///d:/Data/AUDITORIA_CONTINUIDAD.md), [PLAN_POC_PM_LITE.md](file:///d:/Data/PLAN_POC_PM_LITE.md), [RESULTADOS_POC_PM_LITE.md](file:///d:/Data/RESULTADOS_POC_PM_LITE.md), [RESULTADOS_APLICACIONES_REALES.md](file:///d:/Data/RESULTADOS_APLICACIONES_REALES.md) y [CONTINUIDAD_DESARROLLO.md](file:///d:/Data/CONTINUIDAD_DESARROLLO.md).

> **Regla de interpretación:** este documento **complementa y sustituye** las decisiones previas de v0.1 y v0.2.1 donde existan discrepancias o precisiones técnicas derivadas de las PoCs. PortableManager (PM) es la aplicación principal; PM-Desktop (PMD) es únicamente su vista temporal. Contraer PMD **nunca** equivale a cerrar PM.

---

## 18. Principio de producto y alcance

- PM Lite es un entorno portátil, pequeño, discreto y de intervención mínima. Su propósito es permitir usar aplicaciones y documentos del USB conservando sus datos allí y **reduciendo al mínimo los rastros de información personal en Windows anfitrión**, particularmente en entornos compartidos como cibercafés.
- PM **no** es un sistema de diagnóstico, auditoría ni administración general de Windows. No presenta asistentes, informes ni advertencias por cada incidencia. El usuario decide si restablece, reinstala o elimina una aplicación; PM no borra automáticamente sus **datos portables** porque una aplicación falle.
- Se aprovechan comportamientos ya resueltos por PortableApps.com. Los lanzadores **PAF oficiales administran su propia portabilidad y limpieza**; PM no les superpone un segundo motor de restauración. PM gestiona la portabilidad de aplicaciones instaladas desde ZIP/7z/RAR/TAR.
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
- Para los ZIP no se implementa en Lite un clasificador sofisticado de archivos de perfil. Se respeta la estructura del paquete y se conserva el perfil externo administrado por PM. Un ZIP que contenga un archivo de configuración en el propio directorio del programa **podría sobrescribirlo**: no prometer conservación universal de contenidos mezclados.
- El color del **icono de instalación** es **verde** si puede iniciarse la operación y **rojo** si, por ejemplo, la aplicación destino está activa o sus archivos no pueden actualizarse. El color comunica disponibilidad, **no seguridad**. Una aplicación activa no se cierra automáticamente para actualizar.
- Se admite detección de errores y reversión de elementos creados por una instalación fallida; no introducir diagnósticos ni asistentes. La barra de instalación empieza verde y cambia a roja mientras se revierte un error.

### 19.3 Ejecutable principal y criterio de selección

- En PAF se utiliza el **lanzador PortableApps**, no el EXE interno de `App/`.
- En un ZIP con **un único EXE en la raíz**, se registra automáticamente como ejecutable principal.
- En un ZIP con **varios EXE en la raíz**, PM ofrece selección de ejecutable principal; la elección queda en el INI y puede editarse después.
- El usuario decide si el paquete es portable; PM no certifica ni audita la seguridad o portabilidad absoluta de binarios desconocidos.

---

## 20. Clasificación de aplicaciones por colores

- **Blanco/gris:** aplicación sin verificar o recién añadida sin ejecución previa.
- **Verde:** aplicación validada empíricamente que no deja residuos personales en el anfitrión (mecanismo nativo portable, Prioridad 1). Ejemplo: `SumatraPDF`.
- **Amarillo:** aplicación que escribe en rutas o claves conocidas del anfitrión pero queda completamente restaurada mediante las rutinas estándar de PM (Prioridades 2, 3 o recetas de Registro). Ejemplos: `PuTTY` (Registro), `FirefoxPortable` (PAF integrado).
- **Rojo:** aplicación que deja residuos fuera de alcance, utiliza servicios del sistema, controladores, o rutas ambiguas que PM no puede aislar ni restaurar de forma determinista y segura. Se permite su ejecución bajo advertencia explícita al usuario, pero PM no garantiza portabilidad.

---

## 21. Estrategia de Prioridades para Aislamiento de Datos

Para aplicaciones no-PAF (ZIP/directorios), PM evalúa en orden estricto:

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

## 22. Aprendizaje Ligero de Aplicaciones (Validación Fase 4)

PM incorpora un mecanismo heurístico ligero para generar recetas de aplicaciones ZIP desconocidas:
1. **Inspección de metadatos PE:** Extracción de cadenas `CompanyName` y `ProductName` mediante la API estándar de Windows `version.dll` (`GetFileVersionInfoW` / `VerQueryValueW`).
2. **Identificación de candidatos:** Generación de rutas candidatas exclusivas en `%APPDATA%\<Company>\<Product>`, `%LOCALAPPDATA%\<Company>\<Product>` y `HKCU\Software\<Company>\<Product>`.
3. **Línea base previa y respaldo:** Si existen datos del anfitrión en dichas rutas exclusivas, se respaldan como `.pmbkp`.
4. **Cosecha y receta:** Al cerrar la aplicación, los datos modificados se mueven a `Data/apps/<id-estable>/` en el USB y se genera una receta `.ini` con **rutas 100% relativas a variables estándar**.
5. **No interferencia:** Modificaciones concurrentes en carpetas no exclusivas o compartidas (ej. `%TEMP%`, `HKCU\Software\Microsoft\...`) **no se tocan ni se eliminan**, preservando la integridad del anfitrión.

---

## 23. Ciclo de Vida y Protocolo de Cierre (Validación Fase 5)

PM implementa tres modos deterministas de finalización:

### 23.1 Cierre Normal (`NORMAL`)
- El usuario solicita salir de PM o de la aplicación.
- Cierre cooperativo (`WM_CLOSE` / `WM_QUIT`) a las ventanas principales de los procesos del Job Object.
- Cosecha y persistencia de datos y perfiles al USB.
- Desmantelamiento de Junctions (`RemoveDirectoryW`).
- Restauración transaccional de claves de registro desde respaldo `.pmbkp`.
- Restauración de carpetas `.pmbkp` del anfitrión.
- Salida limpia de PM y desactivación del monitor PMGuard.

### 23.2 Cierre Rápido (`FAST`)
- El usuario ordena cierre forzado o expulsión inminente.
- Envío de señal de cierre con temporizador estricto (timeout de 200 ms).
- Si los procesos no terminan en el tiempo límite, llamada inmediata a `TerminateJobObject(hJob, 0)`.
- Cosecha rápida de datos esenciales al USB.
- Restauración prioritaria del anfitrión (Junctions y Registro).

### 23.3 Cierre Crítico de Emergencia (`CRITICAL`)
- Activado por **desconexión física abrupta del USB** o por **caída inesperada del proceso `pm.exe`**.
- El USB ya no es accesible o no es confiable: **se omiten todas las escrituras al USB** para evitar corrupción o bloqueos de E/S.
- Terminación inmediata de procesos huérfanos mediante `TerminateJobObject`.
- PMGuard (ejecutándose desde `%TEMP%`) toma el control mediante la adquisición del Mutex de sesión.
- Eliminación de Junctions huérfanas en el anfitrión.
- Restauración del Registro y carpetas del anfitrión desde los respaldos `.pmbkp` locales.
- **Protocolo de Residuos no Restaurables:** Si un archivo o clave no puede restaurarse (ej. por bloqueo de antivirus):
  - Los respaldos `.pmbkp` se mantienen estrictamente intactos.
  - Se genera el archivo `%TEMP%\pmlog` (texto plano, sin extensión).
  - Se registran exclusivamente las rutas afectadas y los códigos Win32 de error.
  - PM y PMG finalizan sin bloquear el sistema.
