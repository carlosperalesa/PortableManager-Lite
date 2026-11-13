# Future Features — PortableManager Lite

Registro de mejoras arquitectónicas y funcionales postergadas intencionalmente para futuras versiones fuera del alcance actual de Lite.

## 1. Descubrimiento dinámico de rutas de Registro no estándar (más allá de `Company\Product` en PE)

**Contexto:**
Actualmente `LearningEngine` infiere candidatos de Registro a partir de los metadatos PE (`CompanyName` / `ProductName`) hacia `HKCU\Software\<Company>\<Product>` y `HKCU\Software\<Product>`. Sin embargo, aplicaciones portables reales usan a menudo rutas distintas a su `ProductName` o `CompanyName`:
- Ejemplo real: **PuTTY** (`SimonTatham\PuTTY`, no `PuTTY\PuTTY`).
- Instaladores o suites con múltiples subclaves o GUIDs.

**Propuesta futura:**
1. En lugar de depender únicamente de metadatos PE, capturar un delta ligero de `HKCU\Software` (y opcionalmente `HKCU\Environment`) durante la primera ejecución supervisada en `LearningEngine`, o emplear un filtro/notificación (`RegNotifyChangeKeyValue` / snapshot acotado por timestamp de subclave) para descubrir las claves reales modificadas por el Job.
2. Registrar las claves descubiertas en el `wrapper.ini` de la aplicación para que tanto la limpieza como la persistencia opcional (`PersistRegKey=true`) funcionen sin edición manual en aplicaciones como PuTTY.

## 2. Firma Authenticode y endurecimiento frente a AppLocker / SmartScreen

- Firmar `pml.exe` con un certificado Authenticode válido (y mantener el recurso `VERSIONINFO` ya incorporado) para eliminar falsos positivos heurísticos.
- Evaluar un modo opcional compatible con políticas AppLocker estrictas que bloquean ejecutables en `%TEMP%` (donde hoy reside la copia sombra `PMG` cuando se ejecuta desde USB).
