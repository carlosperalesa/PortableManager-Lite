# Fase 06 — Aprendizaje Ligero (LearningEngine)

**Estado: adecuación pendiente; no aprobada ni fusionada.**

## Decisión vinculante del propietario (2026-11-12)

Las recetas de aprendizaje sirven **únicamente para identificar los archivos, las carpetas y las claves de Registro que una aplicación portable efectivamente afecta, y describir las operaciones necesarias para revertir esos cambios durante la limpieza**.

Una receta no constituye ni solicita una copia de seguridad de la aplicación; no guarda perfiles, configuraciones, ni datos de uso en `Data/apps/`. La aplicación es responsable de mantener su configuración mediante su propio modo portable, sus parámetros o las redirecciones autorizadas de PM.

Distinguir:

- **Receta persistente:** inventario de recursos observados (con variables de entorno y rutas portables relativas), tipo de modificación, existencia previa y regla inversa segura. No almacenar rutas absolutas del anfitrión ni contenidos personales.
- **Manifiesto temporal de sesión:** instrucciones write-ahead concretas y estado de las operaciones para PM y PMGuard en `%TEMP%`. Es la fuente operativa para recuperar una sesión activa y no depende de una receta guardada en el USB.
- **Respaldo operativo del anfitrión:** `.pmbkp` para directorios exclusivos y `.reg` bajo el directorio temporal de sesión, únicamente para restaurar el estado anterior del equipo. Conservarlos ante fallo; no publicarlos como datos de la aplicación.
- **Datos internos de la aplicación:** permanecen bajo control de ella o de las estrategias de portabilidad ya autorizadas; LearningEngine no los cosecha para elaborar recetas.

## Aprendizaje permitido

1. Examinar metadatos PE (`CompanyName`, `ProductName`) con `GetFileVersionInfoW` / `VerQueryValueW` para proponer candidatos acotados bajo `%APPDATA%`, `%LOCALAPPDATA%` y `HKCU\Software`.
2. Acreditar exclusividad de recursos antes de modificar o restaurar. Los candidatos de metadatos no prueban por sí solos que se hayan tocado.
3. Comparar el estado anterior con el posterior de forma segura y registrar solamente recursos efectivamente afectados, distinguiendo nuevos, modificados y eliminados; no atribuir escrituras de terceros.
4. Documentar las operaciones inversas necesarias y revalidar recursos antes de ejecutarlas. Terminar y vaciar primero los Jobs, después retirar redirecciones y restaurar el anfitrión.
5. Preservar cambios ambiguos o compartidos; nunca realizar limpieza destructiva basada solo en una inferencia.

## Cierre crítico

Ante caída de PM o pérdida física del USB, PMGuard termina los Jobs y restaura el anfitrión desde el manifiesto y los respaldos de sesión accesibles en `%TEMP%`. No intenta escribir, cosechar ni guardar configuraciones en el USB desconectado. Tampoco necesita leer las recetas persistentes durante la restauración de la sesión ya iniciada.

## Estado del código y validación pendiente

El commit anterior `396e734` pasó seis escenarios sintéticos y las regresiones 0B–5 en Windows, junto a la compilación cruzada Linux; ese resultado corresponde al **comportamiento anterior** y no valida esta nueva definición.

La implementación existente aún puede crear `Data/apps/<id>/<SessionID>/`, exportar valores nuevos del Registro y escribir referencias de datos en la receta. Debe **refactorizarse** para limitarse a descubrimiento y reglas de limpieza, manteniendo el funcionamiento de los subsistemas aprobados. Se necesitan nuevas pruebas para observación de recursos realmente modificados, ausencia de cosecha, restauración exacta de claves preexistentes, exclusión de cambios ambiguos y cierre crítico sin USB.

No cerrar ni fusionar el PR #5 hasta que código, pruebas y CI cumplan estas directrices.
