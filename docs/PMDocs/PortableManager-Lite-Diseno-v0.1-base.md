# PortableManager Lite — Documento de diseño funcional

**Versión:** borrador 0.1  
**Fecha:** 10 de noviembre de 2026  
**Estado:** decisiones de producto consolidadas; no es una especificación técnica cerrada ni una implementación.  
**Alcance:** PM Lite para Windows, principalmente en dispositivos USB y cuentas sin administrador.

> **Principio rector:** PortableManager no debe parecer un explorador de archivos tradicional ni una plataforma de aplicaciones con menús permanentes. Es una píldora de interacción mínima y un escritorio temporal de vidrio; el contenido se ejecuta o abre desde el USB y PM supervisa su ciclo de vida.

## 1. Objetivo

PM Lite administra aplicaciones portables en `App/`, abre y gestiona contenido de `Other/` mediante aplicaciones portables y minimiza los residuos verificables que deja en el anfitrión. Incluye instalación, actualización, lanzamiento, cierre, limpieza y desinstalación; no incluye IA, cifrado ni sincronización de nube en esta edición.

### 1.1 Restricciones

- Debe funcionar como usuario estándar en la mayor cantidad posible de equipos Windows, **sin requerir administrador ni instalación de controladores**.
- No depende de WebDAV, WinFsp, Dokany ni unidades virtuales para ejecutar aplicaciones de Lite.
- No modifica ni elimina indiscriminadamente perfiles, registros o archivos preexistentes del anfitrión.
- Los ejecutables se ejecutan desde carpetas físicas del dispositivo, conservando la estructura PortableApps cuando corresponda.
- PM no promete **cero residuos**: registra observaciones, ejecuta recetas comprobadas y declara lo que no pudo limpiar.
- PM Lite no ejecuta comandos ni instaladores elevados como parte del flujo ordinario.

## 2. Estructura de almacenamiento

```text
<raíz USB>/
├── pm.exe
├── App/
│   ├── .temp/                  # Preparación transaccional de instalaciones
│   ├── FirefoxPortable/
│   │   ├── FirefoxPortable.exe
│   │   ├── App/
│   │   ├── Data/
│   │   └── Other/
│   ├── NotepadPlusPlusPortable/
│   └── <otras aplicaciones>/
├── Data/                       # Datos internos de PM, NO perfiles de usuario por separado
│   ├── config.ini
│   ├── apps.ini                 # Registro de apps, alias y asociaciones
│   ├── recipes/
│   ├── logs/
│   └── session.journal
└── Other/                      # Documentos y carpetas mostrados en PM-Desktop
    ├── Documentos/
    ├── Factura.pdf
    └── Fotos/
```

**Nota de diseño:** las carpetas `Data/` y `Other/` de *cada PortableApp* no son la carpeta raíz `Data/` o `Other/` de PM. La primera conserva los perfiles/configuración de la aplicación; la segunda son documentos del usuario. En Lite no se crean perfiles separados ni cifrados. Ningún archivo de `App/` se representa como carpeta en PM-Desktop: se muestra solo el icono de la aplicación registrada.

## 3. Isla (píldora)

### 3.1 Estado inactivo

- Píldora horizontal negra, compacta, aproximadamente **cinco diámetros de LED** de anchura; contiene **un LED** y ningún texto ni iconos permanentes.
- El LED muestra el estado previsto en el diseño del producto: estados de autenticación cuando apliquen y estado operativo. En Lite se definirá el significado exacto de colores según eventos, sin simular un cifrado inexistente.
- No hay botón permanente de buscar, escritorio, aplicaciones ni cerrar.
- La isla permanece disponible sobre las ventanas según configuración; posición y atajo configurables.

### 3.2 Activación de comandos

- Al invocar la isla con un **atajo global configurable**, queda inmediatamente en modo escritura, sin presionar un segundo botón.
- Si la isla está expandida como Desktop, invocar búsqueda **contrae Desktop y abre la entrada de comandos**.
- El campo se ensancha solo mientras se utiliza.
- El texto tecleado y la sugerencia autocompletada se distinguen visualmente.
- `Esc` cancela búsqueda, borra la entrada activa y devuelve la isla a reposo.
- Con la entrada **vacía**, `Espacio` abre PM-Desktop. El espacio también puede formar parte de los parámetros de una MagicWord cuando la entrada **no** está vacía.

### 3.3 Autocompletado SlickRun

- Coincidencia por **prefijo**, no búsqueda difusa ni coincidencia en cualquier parte.
- Candidatos: nombres de aplicaciones, alias (MagicWords), comandos internos y archivos/carpetas visibles de `Other/` según la búsqueda. **Las aplicaciones se presentan primero**, archivos/carpetas después.
- Ejemplo: escribir `f` muestra `firefox` como primera sugerencia si corresponde; presionar `Enter` ejecuta esa **primera coincidencia autocompletada**, aunque el usuario solo haya tecleado `f` o `fire`.
- `ff` y `firefox` pueden resolver a la misma aplicación, sin alterar su nombre de instalación, nombre de archivo ni carpeta.
- Flechas permiten cambiar de selección; `Enter` ejecuta la opción actualmente resaltada. El uso principal sigue siendo **dos o pocas teclas + Enter**.
- No se guarda historial de consultas/comandos ni se usa historial para autocompletar; **sí** se completa desde la biblioteca estática de MagicWords, alias, aplicaciones y archivos actualmente disponibles.
- Orden inicial propuesto: coincidencia exacta escrita, luego coincidencias por prefijo de aplicaciones/alias, luego elementos de `Other/`; ordenar empates de forma estable y configurable en el futuro. **Pendiente:** definir colisiones de alias y precedencia definitiva.

### 3.4 Acción web visible

- Mientras hay texto en la búsqueda, puede aparecer **un icono contextual de navegador** junto al campo, sin texto adicional.
- Hacer clic en él **busca el texto literalmente escrito**, no la sugerencia autocompletada, usando el navegador portable predeterminado y el buscador configurado (Google inicialmente).
- Así, `fire` + `Enter` abre Firefox si es la primera MagicWord, mientras `fire` + clic en el icono web busca **«fire»**, sin ejecutar la MagicWord.
- Si no existe navegador portable configurado, se aplica la política de confirmación para usar el anfitrión; no se ejecuta silenciosamente el navegador del host.

## 4. MagicWords y `_DEFAULT`

El comportamiento se basa en SlickRun, separando **coincidencia y ejecución** de la acción web explícita.

### 4.1 MagicWords

- Cada MagicWord tiene identificador, uno o más nombres/alias, acción ejecutable y parámetros opcionales.
- Alias separados por comas; por ejemplo `ff,firefox` apunta a un único `FirefoxPortable.exe`.
- Admitir parámetros después del nombre: `ff https://ejemplo.com` abre esa URL con FirefoxPortable.
- Las rutas se resuelven respecto de la raíz de PM, no respecto de una letra de unidad fija.
- La configuración de MagicWords nunca cambia el nombre físico de la aplicación ni sus archivos.
- Comandos internos iniciales: `exit` (cierre normal de PM), calculadora con prefijo `=`, y aquellos necesarios para gestión/ajustes que se definan más adelante.
- `=25*4` devuelve `100` sin invocar un intérprete de comandos del sistema ni `eval` inseguro. Solo aritmética expresamente admitida.

### 4.2 `_DEFAULT` tal como SlickRun

- `_DEFAULT` es una **MagicWord de respaldo** que se ejecuta cuando, al confirmar con `Enter`, el texto **no resuelve a una MagicWord/comando/ruta/archivo reconocidos**.
- Recibe **la entrada original completa**, sin reemplazarla por una autocompletación.
- La expansión de parámetros conserva la distinción de SlickRun: `$I$` = texto original; `$W$` = texto codificado para URL. En el caso predeterminado, `$W$` se incorpora a la consulta de Google.
- Valor inicial: abrir `https://www.google.com/search?q=$W$` en el **navegador portable predeterminado** de PM.
- La MagicWord `_DEFAULT` es configurable en el INI; no se limita conceptualmente a Google y puede apuntar a otra acción legítima.
- No debe lanzar de manera implícita binarios arbitrarios, una shell del host o comandos elevados como resultado de texto no reconocido.
- Una coincidencia parcial ya autocompletada se considera MagicWord válida al pulsar Enter, por lo que **no cae en `_DEFAULT`**.
- Diferencia deliberada con SlickRun: PM restringe ejecución y apertura al entorno portable, salvo autorización explícita para usar el anfitrión.

**Ejemplos:**

| Entrada y acción | Resultado |
|---|---|
| `f` + Enter | Ejecutar primera MagicWord coincidente (ej. Firefox) |
| `ff` + Enter | Ejecutar FirefoxPortable |
| `ff https://ejemplo.com` + Enter | FirefoxPortable con esa URL como parámetro |
| `fire` + clic en icono web | Buscar literalmente «fire» en el navegador portable |
| `una pregunta sin MagicWord` + Enter | Ejecutar `_DEFAULT` con el texto completo |
| `=20/4` + Enter | Resultado aritmético `5` |
| `exit` + Enter | Cierre normal de PM |
| entrada vacía + Espacio | Abrir PM-Desktop |

## 5. PM-Desktop (PMD)

### 5.1 Apariencia y tamaño

- Es una **isla/panel de vidrio transparente (glass)** flotante sobre la pantalla/monitor activo, **no** un escritorio completo ni una ventana convencional con título y botón cerrar.
- Contiene únicamente iconos de **aplicaciones registradas en `App/`** y de **archivos o carpetas contenidos directamente en `Other/`**. La carpeta raíz `App/`, la carpeta `Data/` y la carpeta raíz `Other/` no se muestran como iconos técnicos.
- Los iconos de aplicaciones proceden del lanzador/metadata; los archivos y directorios se representan con sus tipos e iconos correspondientes.
- Dimensión automática: con un icono, la superficie ocupa aproximadamente una celda y su margen; se expande conforme aparezcan elementos.
- `Columns` **no es mínimo**: si se configura `Columns=4`, es el **máximo de cuatro columnas**, rellenando hacia abajo. Si se omite, PM distribuye horizontalmente hasta el espacio útil de la pantalla activa y luego envuelve filas. Nunca debe desbordar la pantalla.
- Número de filas se deriva del contenido; opciones avanzadas de tamaño/orden podrán añadirse al panel de control en una versión posterior. En Lite inicial basta con INI editable.
- Se adapta a cambios de monitor, resolución y escalado; no asume una ubicación absoluta fija.

### 5.2 Interacción

- `Espacio` desde la isla en reposo expande PMD; desde PMD, se puede activar búsqueda y PMD se contrae inmediatamente.
- **Un clic izquierdo** abre aplicación, archivo o carpeta; no se exige doble clic.
- Abrir aplicación o archivo contrae automáticamente PMD.
- Abrir carpeta navega dentro del panel, mostrando su contenido; `Esc` sube al directorio anterior; en raíz, `Esc` contrae PMD.
- Clic fuera del panel lo contrae. PM continúa activo y los procesos lanzados continúan ejecutándose.
- Flechas arriba/abajo/izquierda/derecha mueven la selección; `Enter` abre la selección.
- Clic derecho en aplicación: **Abrir, Cerrar, Configurar, Desinstalar**. `Configurar` incluye nombre visible, alias, extensiones asociadas y opciones disponibles de receta de limpieza.
- **Cerrar aplicación desde PMD** hace cierre ordinario + limpieza unitaria; **no** ofrece modos normal/rápido/crítico en ese menú.
- No hay papelera ni se desinstala arrastrando iconos.
- Clic derecho en archivos/carpetas puede ofrecer operaciones de archivo, por definir; debe impedir borrados involuntarios.

## 6. Asociaciones de extensiones y apertura de archivos

- Al instalar una aplicación, PM registra las extensiones que **ella declara o que se hayan identificado de manera fiable**; no asume que todos los formatos asociados en Windows sean soportados por esa aplicación portable.
- Se pueden editar manualmente en `Data/apps.ini` y, más adelante, desde Configurar.
- Una extensión puede tener varias aplicaciones candidatas; una queda como predeterminada de PM. Una instalación nueva **no sobrescribe silenciosamente** una elección existente.
- Los archivos de `Other/` se abren, por defecto, con una aplicación **instalada en PM**, pasando la ruta del archivo como parámetro de forma segura.
- Si no existe asociación, PM ofrece: **No abrir / Abrir con otra aplicación portable / Abrir con aplicación predeterminada del anfitrión**.
- Al elegir otra aplicación portable, se puede marcar **Recordar para esta extensión**, persistiendo la nueva preferencia.
- Usar la app predeterminada del anfitrión requiere confirmación explícita; PM informa que no puede garantizar la limpieza ni la portabilidad de esa app externa.
- Si la aplicación asociada está desinstalada o no disponible, se vuelve a presentar el selector en lugar de ejecutar una ruta inválida.
- Abrir `.zip` como documento y **instalar** `.zip` como paquete portable son operaciones distintas y coexistentes.
- Las rutas de documentos se pasan como argumentos debidamente delimitados: no se construyen comandos de shell por concatenación.

## 7. Detección de instalables desde el portapapeles

### 7.1 Experiencia visual

- Al copiar al portapapeles un **archivo real** con extensión `.paf.exe`, `.zip`, `.7z`, `.rar` o `.tar`, la isla expande brevemente su contenido contextual: **`[icono instalar] [nombre de aplicación candidato] [LED]`**.
- Para `app_v01.02.zip`, el nombre visible sugerido puede ser `app`. El usuario ve a qué aplicación candidata apuntaría la operación.
- **Un clic en el icono instalar inicia la instalación o actualización**; no se abre una segunda pantalla de confirmación en el caso habitual.
- Si no se pulsa el icono, no pasa nada: el usuario puede seguir trabajando, abrir el archivo normalmente o sobrescribir el portapapeles.
- No hay instalación automática, ni botón separado para cancelar la oferta, ni detección invasiva del contenido de archivos copiados para decidir si mostrar el icono.
- Copiar un archivo instalable es una **oferta**, no una orden de ejecución. El sistema no instala sin clic.

### 7.2 Elección manual de instalación/actualización

- El nombre sugerido permite reconocer el destino. **Pendiente de detalle visual:** cómo cambia el usuario el destino de una actualización cuando el nombre inferido sea erróneo. Propuesta para iterar: clic en el nombre abre un selector compacto de aplicaciones existentes; clic en instalar usa el destino actualmente mostrado.
- Cuando un paquete se reconoce con identidad inequívoca y ya existe, PM lo trata como **actualización**; cuando el usuario cambia el destino manualmente, esa elección tiene prioridad.
- PM no decide que dos archivos son la misma aplicación únicamente porque sus nombres se parezcan: `app101.zip` y `app102.zip` pueden corresponder a versiones o a paquetes distintos.
- El usuario acepta expresamente el riesgo de elegir actualizar una app incorrecta, pero PM todavía debe impedir sobrescribir datos ajenos por accidente y registrar lo que cambió.
- La fuente y la ruta exacta siempre deben estar dentro de la interacción accesible (por ejemplo, información al pasar el cursor), aunque la isla permanezca sin texto permanente.

## 8. Motor de instalación y actualización

### 8.1 Paquetes admitidos

- **PortableApps `.paf.exe`:** invocar el **instalador oficial individual**, no simplemente desempaquetarlo como ZIP. Respetar su detección de una instalación anterior, estructura `App/Data/Other`, políticas de conservación y eventuales diálogos del propio instalador.
- **ZIP, 7z, TAR, RAR:** usar extractores portables disponibles para descomprimir a una zona aislada; detectar raíz de aplicación, ejecutable principal, manifiesto y posible actualización sin reconfigurar el sistema anfitrión.
- No convertir automáticamente instaladores Windows genéricos `.exe` en aplicaciones portables.
- Un `.paf.exe` puede ser instalador online o requerir licencia/intervención. Si no admite flujo silencioso confiable, PM puede ofrecer el diálogo oficial: **la ausencia de pantalla extra es objetivo de UX, no motivo para saltarse licencias ni suponer instalación desatendida**.

### 8.2 Carpeta de trabajo y transacciones

- Preparar los comprimidos en `App/.temp/<operación>/`, dentro del USB; verificar espacio libre antes de extraer.
- Validar rutas antes de extraer: bloquear `../`, rutas absolutas, enlaces peligrosos, archivos que escapen de la carpeta temporal, duplicados sospechosos y límites de tamaño descomprimido.
- Distinguir nueva instalación y actualización. Actualizar requiere que la app esté cerrada y sus archivos no estén en uso.
- Mantener un respaldo mínimo de archivos que se reemplazarán y un manifiesto en `session.journal` para poder recuperar la operación si falla.
- Tras validación, publicar el resultado en `App/<ID>/`; limpiar la carpeta temporal **solo si es inequívocamente propia y ya no se necesita para recuperar**.
- Si falla, la isla se vuelve **roja**. PM intenta revertir cambios propios y **no anuncia instalación exitosa** hasta verificar el estado final. Si revertir falla, conserva evidencia y deja estado «requiere reparación», nunca borra arbitrariamente datos anteriores.
- **El progreso es real**: bytes extraídos/validados, archivos procesados u operaciones completadas. Si el instalador externo no expone progreso fiable, presentar actividad indeterminada sin simular porcentajes.

### 8.3 Particularidades PAF

- El instalador de PortableApps.com puede preservar `Data` al actualizar, reemplazar `App`/`Other` según metadatos y ejecutar lógica propia. PM no debe sustituir a ciegas esta política por una sobreescritura genérica.
- Detectar el directorio padre correcto de instalación y si existe la misma aplicación; verificar destino real **después** del instalador, ya que una PoC anterior mostró que los parámetros silenciosos intentados no respetaron el destino solicitado.
- Dado que el propio `.paf.exe` es responsable de parte del proceso, PM registra PIDs, cambios observables y resultado, sin prometer reversión atómica perfecta de un instalador externo.

## 9. Lanzamiento de aplicaciones

- Ejecutar el **lanzador portable registrado**, no el binario interno, cuando el paquete lo proporcione (por ejemplo, `FirefoxPortable.exe`).
- Resolver desde el directorio real de PM; no depender de una letra USB permanente.
- Pasar parámetros y rutas sin expansión de shell insegura; definir directorio de trabajo apropiado.
- Rastrear los procesos lanzados y sus descendientes cuando sea posible, distinguiéndolos de procesos del usuario/anfitrión con el mismo nombre.
- Cada aplicación mantiene su propio `Data/` si así lo define su distribución; Lite no introduce perfiles cifrados ni intercambiables.
- Redirigir variables de proceso `%TEMP%` / `%TMP%` hacia una carpeta controlada de PM cuando sea compatible, sin asumir que todas las aplicaciones las respetarán.

## 10. Cierre, progreso y limpieza

### 10.1 Cierre de una aplicación

- Desde PMD, menú contextual **Cerrar**: solicitar cierre ordinario de esa app, esperar los procesos relevantes y aplicar su receta **unitaria** de limpieza/restauración.
- Cerrar la ventana del programa por sus propios medios debe también permitir a PM detectar el fin de sesión y ejecutar limpieza cuando corresponda.

### 10.2 Cierre de PM

- **Solo clic derecho sobre la píldora** (o atajos configurables) ofrece **Normal / Rápido / Crítico**.
- Normal: solicita cierre ordenado, espera, aplica recetas, verifica restauraciones, consolida el diario.
- Rápido: reduce esperas y labores opcionales; **no** omite restauraciones esenciales ya comprometidas.
- Crítico: prioriza parar la sesión y registrar pendientes para recuperación; puede dejar operaciones incompletas y debe declararlo, no afirmar que el host quedó limpio.
- MagicWord `exit`: cierre **normal** de PM por defecto.

### 10.3 Indicador de progreso

- No hay barra circular ni spinner de llenado ficticio. **La píldora negra completa** se llena cromáticamente **de izquierda a derecha** de acuerdo con trabajo medible.
- En operaciones sin total verificable, usar estado de actividad distinguible **sin porcentaje falso**.
- Error: la píldora se vuelve **roja**; ante instalación fallida se intenta revertir los cambios propios antes de volver al reposo o mostrar estado de reparación.
- No confundir éxito de proceso con limpieza exitosa: verificar terminación y registros antes de señalar éxito.

### 10.4 Residuos del anfitrión

- Inventario inicial por aplicación: archivos bajo rutas conocidas, `%APPDATA%`, `%LOCALAPPDATA%`, `%TEMP%`, `%TMP%`, claves HKCU que se puedan inspeccionar y otros lugares alcanzables sin privilegios.
- Recetas por aplicación: identificar, respaldar si corresponde, revertir **solo cambios atribuibles** y verificar. Si existían datos previos, restaurarlos sin pisar cambios legítimos ajenos.
- Priorizar limpieza del estado que PM controló; no prometer borrar Prefetch, Amcache, caché de WebClient, registros del sistema u otros lugares no accesibles.
- Categorías de evaluación: verde (sin residuos relevantes detectados bajo alcance documentado), amarillo (residuos conocidos/receta), rojo (riesgos o limpieza insuficiente), gris (no evaluado). Las categorías no son garantías absolutas.
- Registrar los resultados y elementos pendientes para `session.journal`.

## 11. Desinstalación

- Desde el menú contextual de la app en PMD: **Desinstalar**.
- Si está abierta, cierre ordinario y limpieza unitaria antes de proceder.
- Eliminar **solo** los archivos dentro de la carpeta de esa aplicación que pertenezcan a la instalación de PM y las configuraciones registradas por PM, sin borrar datos de otras aplicaciones ni el contenido de `Other/` raíz.
- Al desinstalar una PortableApp, su carpeta `Data/` contiene datos del usuario; **pendiente de decisión:** si eliminar junto con la app o permitir conservar/exportar. No se decide silenciosamente.
- Eliminar su icono, alias y asociaciones; reestablecer asociación alternativa si existe.
- Diario y operación recuperable ante fallo, tanto como permita el sistema de archivos.

## 12. Configuración INI de referencia

Este formato es un **borrador legible**, no un esquema definitivo ya implementado.

```ini
[PM]
Edition=Lite
Root=.

[Island]
Hotkey=Win+Q
IdleWidthInLedUnits=5
SingleLed=true

[Search]
PrefixOnly=true
AutocompleteMagicWords=true
AutocompleteFiles=true
HistoryEnabled=false
AppsFirst=true
ShowWebAction=true

[MagicWord._DEFAULT]
Action=WebSearch
BrowserAppId=firefoxportable
URL=https://www.google.com/search?q=$W$

[MagicWord.Calculator]
Prefix==
Action=Calculate

[MagicWord.Exit]
Names=exit
Action=ShutdownNormal

[App.firefoxportable]
Name=Firefox
Aliases=ff,firefox
Launcher=App/FirefoxPortable/FirefoxPortable.exe
Extensions=html,htm,pdf
IsDefaultBrowser=true

[App.notepadplusplusportable]
Name=Notepad++
Aliases=npp,notepad
Launcher=App/NotepadPlusPlusPortable/NotepadPlusPlusPortable.exe
Extensions=txt,ini

[App.7zipportable]
Name=7-Zip
Aliases=7z,zip
Launcher=App/7ZipPortable/7-ZipPortable.exe
Extensions=zip,7z,rar,tar

[Associations]
html=firefoxportable
htm=firefoxportable
pdf=firefoxportable
txt=notepadplusplusportable
ini=notepadplusplusportable
zip=7zipportable

[Desktop]
Style=Glass
SingleClick=true
AutoSize=true
; Columns omitido = ajustar al ancho útil de pantalla
; Columns=4
CloseOnOutsideClick=true
CloseOnLaunch=true
EscapeGoesUp=true

[Install]
ClipboardOffer=true
OneClickInstall=true
TempDirectory=App/.temp
```

La presencia de Firefox, Notepad++ y 7-Zip en este INI es **ilustrativa**: las entradas se crean únicamente si las aplicaciones están realmente instaladas y sus lanzadores fueron verificados. La lista de extensiones es editable y no constituye evidencia de capacidad nativa de cada app. Las MagicWords y las asociaciones comparten IDs estables de aplicaciones, nunca dependen del nombre visual ni de los alias.

## 13. UX: tablas de estados

| Estado | Vista de isla | Acción principal |
|---|---|---|
| Reposo | Píldora negra, un LED | Espera atajo, clic/contexto, portapapeles |
| Búsqueda | Entrada breve con sugerencia y posible icono web | Enter ejecuta candidato; web busca texto literal |
| Desktop | Panel glass con iconos ajustado al contenido | Clic único abre; Esc/clic externo contrae |
| Instalación disponible | `[instalar] [app candidata] [LED]` | Clic en instalar inicia |
| Instalando/actualizando | Píldora como progreso real | No anunciar éxito antes de validar |
| Error | Píldora roja | Revertir cambios controlados, conservar evidencia |
| Cerrando PM | Píldora como progreso de trabajo medible | Ejecutar modo seleccionado |

## 14. Límites del primer lanzamiento y criterios de aceptación

La primera versión funcional debe demostrar al menos:

1. Inicio como usuario estándar desde una carpeta USB, sin elevación, WebDAV o instalación permanente.
2. Isla sin controles permanentes salvo LED; búsqueda por atajo; `f` + Enter abre primera coincidencia; `ff` abre Firefox si está instalado; clic web busca texto original.
3. `_DEFAULT` ejecuta acción de respaldo con `$W$`/`$I$` cuando no existe MagicWord válida; `=...` calcula; sin historial.
4. Espacio abre PMD adaptable; clic único abre; Esc retrocede o contrae; clic externo contrae.
5. PMD muestra iconos de aplicaciones registradas y elementos de `Other/`, nunca carpetas internas de `App/`.
6. Portapapeles ofrece instalación contextual; un solo clic inicia; ignorar la oferta no modifica nada.
7. Instalación `.paf.exe` respeta instalador individual; ZIP/7z/RAR/TAR se preparan en `App/.temp/` y no salen de la zona controlada durante extracción.
8. Asociación de extensiones editable; abrir archivo usa una app portable; elección alternativa puede recordarse; host siempre con consentimiento.
9. Cierre unitario desde PMD y cierre global normal/rápido/crítico desde la píldora, con registro de procesos y limpieza bajo alcance realista.
10. Desinstalación elimina solo la aplicación seleccionada, actualiza registro/alias y no afecta `Other/` raíz.
11. Documentar errores, reversión y residuos observados sin prometer cero rastros.

## 15. Decisiones pendientes de la siguiente iteración

Estas cuestiones **no están resueltas** y no deben completarse automáticamente con supuestos al programar:

1. **Núcleo del instalador:** reglas detalladas para identidad de ZIP/versiones, elección manual de destino desde el nombre mostrado y políticas de sustitución/rollback por formatos.
2. **Actualización incorrecta elegida por el usuario:** grado de confirmación o salvaguardias mínimas antes de sustituir datos existentes.
3. **Desinstalación y datos propios de PortableApps:** si preservar, exportar o eliminar el `Data/` de la app, y qué interacción mínima mostrar.
4. **Motor de búsqueda:** orden estable para varias apps que empiezan igual, conflicto de MagicWords y precedencia exacta entre alias, comandos y archivos.
5. **Navegación PMD:** menú contextual específico para archivos/carpetas, orden de iconos y ubicación inicial del panel glass sobre monitores múltiples.
6. **Framework/implementación:** lenguaje, UI de vidrio, captura global de atajos y seguimiento de procesos, **después de inspeccionar si ya existen componentes del proyecto**.
7. **Progreso de instaladores externos:** cuando `.paf.exe` no comunica bytes/porcentaje, cuál será la animación indeterminada aceptable sin falsificar progreso.
8. **Alcance y límites de recetas de limpieza:** políticas de respaldo y restauración en conflictos con aplicaciones preexistentes del anfitrión.

## 16. Hoja de ruta reservada: Secure y Cloud

**Secure — futuro, no implementar ahora.** Investigar USB convencional con partición pública que ejecuta PM y segunda partición protegida mediante BitLocker To Go; verificar desbloqueo/bloqueo sin administrador y variaciones por Windows/políticas del host. Alternativas futuras: `data.pmv`/`other.pmv` cifrados por archivo. No exigir en Lite tecnología virtual de montaje.

**Cloud — futuro, no implementar ahora.** Analizar rclone/OneDrive, caché local, sincronización, credenciales y disponibilidad sin administrador. No prometer ejecución directa de Firefox mediante WebDAV: la PoC evidenció que el cliente WebDAV de Windows bloquea una DLL requerida mayor que el límite configurado. Plantear descarga/caché controlada y sincronización solo después de medir rendimiento e integridad.

## 17. Evidencias y referencias

**PoC existentes del proyecto (resumidas en conversaciones previas):** Notepad++ portable ejecutado desde rclone WebDAV en una máquina; FirefoxPortable oficial ejecutado exitosamente desde carpeta física; Firefox desde WebDAV falló por `xul.dll` (180.079.232 B), límite WebClient 50.000.000 B, error Win32 223; la PoC registró residuos de HKCU/AppData/ProgramData y confirmó que no debe asumirse ausencia total de rastros.

**Fuentes externas verificadas:**

- SlickRun Advanced Help, evento `_DEFAULT`, parámetros `$W$` y `$I$`: https://www.bayden.com/slickrun/1033/SlickRunAdvancedHelp.htm
- SlickRun Setup, MagicWords, alias y control de autocompletado por origen: https://bayden.com/slickrun/1033/SlickRunSetup.htm
- PortableApps.com: instalación y actualización de aplicaciones portables individuales: https://portableapps.com/support/portable_app
- PortableApps.com Installer: política de carpetas durante actualización (`Data` preservada por defecto, otras configurables): https://portableapps.com/manuals/PortableApps.comLauncher/ref/paf/installer.html

---

**Regla de trabajo para futuras iteraciones:** no programar interfaces o módulos duplicados sin comprobar los archivos existentes; conservar todas las decisiones aprobadas aquí y modificar explícitamente solo los apartados que cambien.
