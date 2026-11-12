# Fase 06 — Aprendizaje Ligero (LearningEngine)

**Estado: Fase 6 aprobada y fusionada en `main` (PR #5).**

## Decisión vinculante del propietario (2026-11-12)

Los wrappers de aprendizaje sirven **únicamente para identificar los archivos, las carpetas y las claves de Registro que una aplicación portable efectivamente afecta, y describir las operaciones necesarias para revertir esos cambios durante la limpieza**.

Un wrapper no constituye ni solicita una copia de seguridad de la aplicación; no guarda perfiles, configuraciones, ni datos de uso en `Data/apps/`. La aplicación es responsable de mantener su configuración mediante su propio modo portable, sus parámetros o las redirecciones autorizadas de PML.

Distinguir:

- **Wrapper persistente:** inventario de recursos observados (con variables de entorno y rutas portables relativas), tipo de modificación, existencia previa y regla inversa segura. No almacenar rutas absolutas del anfitrión ni contenidos personales.
- **Manifiesto temporal de sesión:** instrucciones write-ahead concretas y estado de las operaciones para PML y PMGuard en `%TEMP%`. Es la fuente operativa para recuperar una sesión activa y no depende de un wrapper guardado en el USB.
- **Respaldo operativo del anfitrión:** `.pmbkp` para directorios exclusivos y `.reg` bajo el directorio temporal de sesión, únicamente para restaurar el estado anterior del equipo. Conservarlos ante fallo; no publicarlos como datos de la aplicación.
- **Datos internos de la aplicación:** permanecen bajo control de ella o de las estrategias de portabilidad ya autorizadas; LearningEngine no los cosecha para elaborar wrappers.

## Ubicación del wrapper

`LearningRequest::app_dir` identifica un directorio de instalación existente
relativo a la raíz de PML. Puede contener el ejecutable en una subcarpeta.
El motor valida la ruta del ejecutable dentro de la instalación y rechaza
traversal y puntos de reanálisis. Tras confirmar el rollback, valida de nuevo
la cadena del directorio, mantiene sus handles durante la escritura y actualiza
atómicamente su `wrapper.ini`. Conserva las entradas y los comentarios
preexistentes. Rechaza un archivo enlazado o una carpeta con ese nombre.

Las instalaciones con artefactos del esquema previo pueden conservarlos como
referencia. El motor deja de usar `Data/recipes/` y no migra archivos por ID.
En la futura Fase 8, la actualización por copia encima deberá detectar
`wrapper.ini` en el paquete y preservar el archivo de la instalación.

## Aprendizaje permitido

1. Examinar metadatos PE (`CompanyName`, `ProductName`) con `GetFileVersionInfoW` / `VerQueryValueW` para proponer candidatos acotados bajo `%APPDATA%`, `%LOCALAPPDATA%` y `HKCU\Software`.
2. Acreditar exclusividad de recursos antes de modificar o restaurar. Los candidatos de metadatos no prueban por sí solos que se hayan tocado.
3. Comparar el estado anterior con el posterior de forma segura y registrar solamente recursos efectivamente afectados, distinguiendo nuevos, modificados y eliminados; no atribuir escrituras de terceros.
4. Documentar las operaciones inversas necesarias y revalidar recursos antes de ejecutarlas. Terminar y vaciar primero los Jobs, después retirar redirecciones y restaurar el anfitrión.
5. Preservar cambios ambiguos o compartidos; nunca realizar limpieza destructiva basada solo en una inferencia.

## Cierre crítico

Ante caída de PML o pérdida física del USB, PMGuard termina los Jobs y restaura el anfitrión desde el manifiesto y los respaldos de sesión accesibles en `%TEMP%`. No intenta escribir, cosechar ni guardar configuraciones en el USB desconectado. Tampoco necesita leer los wrappers persistentes durante la restauración de la sesión ya iniciada.

## Estado del código y validación pendiente

El commit anterior `396e734` pasó seis escenarios sintéticos y las regresiones 0B–5 en Windows, junto a la compilación cruzada Linux; ese resultado corresponde al **comportamiento anterior** y no valida esta nueva definición.

La implementación se ha modificado en el PR #5 para sustituir la captura de datos HKCU por comparación temporal de estados y para registrar en el wrapper únicamente los nombres de archivos/carpetas observados y las reglas inversas de Junctions y Registro. **No se copia ningún archivo .reg aprendido al USB** ni se escribe `RegistryDataPath` en el wrapper. Los archivos que la propia aplicación escribe mediante una Junction permanecen en la carpeta portable redirigida como parte de la estrategia de portabilidad existente; no son un respaldo generado por el wrapper.

Validación confirmada en GitHub Actions [43245395915](https://github.com/carlosperalesa/PortableManager-Lite/actions/runs/43245395915): compilación Linux y suite nativa Windows correctas, con Fase 6 6/6, Fase 5 8/8, Fase 4 9/9 y regresiones anteriores. Siguen sin ejecutarse pruebas con aplicaciones reales, ACL personalizadas y desconexión física de USB específica de esta fase.

La revisión técnica y la CI del HEAD final [43246078395](https://github.com/carlosperalesa/PortableManager-Lite/actions/runs/43246078395) fueron favorables. El PR #5 se fusionó como commit `843f1fe75b3bdbd4dd437b6654583e9c3155ba5e`.
