# Future Features — PortableManager Lite

Registro de mejoras arquitectónicas y funcionales postergadas intencionalmente para futuras versiones fuera del alcance actual de esta Beta (`v0.2.2-beta.1`).

## 1. Descubrimiento dinámico de rutas de Registro no estándar (más allá de `Company\Product` en PE)

**Contexto:**
Actualmente `LearningEngine` infiere candidatos de Registro a partir de los metadatos PE (`CompanyName` / `ProductName`) hacia `HKCU\Software\<Company>\<Product>` y `HKCU\Software\<Product>`. Aplicaciones portables reales usan a menudo rutas distintas a su `ProductName` o `CompanyName` (por ejemplo, **PuTTY** usa `SimonTatham\PuTTY`).

**Propuesta futura:**
1. Capturar un delta acotado de `HKCU\Software` (y opcionalmente `HKCU\Environment`) durante la primera ejecución supervisada en `LearningEngine` para descubrir las subclaves reales modificadas por el Job.
2. Registrar las claves descubiertas en el `wrapper.ini` de la aplicación para que tanto la limpieza como la persistencia opcional (`PersistRegKey=true`) funcionen sin edición manual.

## 2. Firma Authenticode y endurecimiento frente a AppLocker / SmartScreen

- Firmar `pml.exe` con un certificado Authenticode válido (manteniendo el recurso `VERSIONINFO`) para eliminar falsos positivos heurísticos y avisos de SmartScreen.
- Evaluar un modo opcional compatible con políticas AppLocker estrictas que bloquean ejecutables en `%TEMP%` (donde hoy reside la copia sombra `PMG` cuando se ejecuta desde USB).

## 3. Gestión avanzada de comandos `[Url.*]` e importación `SlickRun.srl`

- Importador integrado de archivos `.srl` (SlickRun) desde la UI o CLI hacia secciones `[Url.<nombre>]` en `Data\apps.ini`.
- Soporte de múltiples alias separados por coma dentro de un mismo `[Url.*]` o edición/eliminación directa desde el menú contextual de PMD sin abrir `Data\apps.ini`.

## 4. Panel gráfico de configuración en PMD *(Implementado en `v0.2.2-beta.3`)*

- ~~Reemplazar la apertura directa de `Data\config.ini` en el editor de texto por un panel visual integrado para `Hotkey`, `AlwaysOnTop`, `Translucent`, `RememberPosition` y buscador web por defecto (`[Web]`).~~ Implementado mediante el entorno `Config` de PMD y el modal glass de configuración por aplicación.

## 5. Aislamiento avanzado (alcance Pro, fuera de Lite)

- Interceptación/redirección de rutas absolutas con letra de unidad (como las 23 entradas `HKCU` con la ruta del USB detectadas en FirefoxPortable) o virtualización de sistema de archivos cuando el lanzador de la aplicación no las abstrae por sí solo.

## 6. Instalador desde repositorio en línea y actualizador integrado

- Comando tipo `install portableapps firefox` (o catálogo portable) que consulte un repositorio en línea, descargue el paquete con barra de progreso/verificación y lo entregue al instalador de PML.
- Buscador de actualizaciones para las aplicaciones ya instaladas directamente desde PML.
