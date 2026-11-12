# Plan de Ejecución de Pruebas de Concepto (PoC) — PortableManager Lite

**Fecha:** 11 de noviembre de 2026  
**Estado:** Fase 0 completada; plan consolidado a partir de los informes de los Agentes A, B y C.  
**Referencia previa:** [AUDITORIA_CONTINUIDAD.md](file:///d:/Data/AUDITORIA_CONTINUIDAD.md)

---

## 1. Principios de Ejecución y Restricciones

1. **No repetir pruebas aprobadas:** No se repetirán las pruebas reales ya aprobadas y documentadas (`D1` FirefoxPortable, `D2` SumatraPDF, `D3` PuTTY, ni el preflight de paquetes originales).
2. **Protocolo secuencial estricto:** Cada PoC seguirá los 8 pasos obligatorios:
   1. Preparar entorno aislado.
   2. Registrar estado inicial.
   3. Ejecutar una única prueba.
   4. Capturar resultados.
   5. Verificar restauración.
   6. Comprobar ausencia de residuos.
   7. Generar informe (Markdown + JSON independiente).
   8. Detenerse y evaluar.
3. **Rutas relativas:** Ninguna configuración persistente de PM utilizará rutas absolutas; todas las rutas persistentes serán relativas a la ubicación del ejecutable y solo se resolverán en tiempo de ejecución.
4. **Protección del anfitrión y del USB:**
   - Solo se usarán carpetas de prueba sintéticas y claves `HKCU\Software\PM_Lite_PoC_Test_...` con nombres únicos.
   - Antes de requerir la conexión o desconexión física del USB (por ejemplo, en la Fase 2), el coordinador se detendrá y solicitará confirmación explícita al usuario.

---

## 2. Entorno Técnico de Compilación en el Anfitrión

La auditoría técnica (Agente B) y de sistema detectó que en el equipo anfitrión están disponibles:
- **Compilador de ejecutables nativos PE en Windows:** `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe` (capaz de generar un único `.exe` autónomo sin dependencias externas en el USB, invocando directamente las APIs Win32 de `kernel32.dll`, `advapi32.dll`, `user32.dll` y `rstrtmgr.dll` mediante P/Invoke, y permitiendo fijar el bit `IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP` `0x0400` / `/SWAPRUN:CD` en la cabecera PE).
- **Python 3.12 (`C:\Users\cperales\AppData\Local\Programs\Python\Python312\python.exe`):** para los arneses de verificación independiente, cálculo de hashes SHA-256, auditoría antes/después y generación de los reportes JSON de cada PoC.

---

## 3. Plan Detallado por Fases

### Fase 1 — PoC Runtime y PMGuard (`poc_fase1_runtime_pmguard.json`)
- **Objetivo:** Comprobar que un único ejecutable (`pm_poc.exe`) con modo normal y modo `--guard` puede ejecutar el cierre de emergencia aunque desaparezca su origen, sin consumir CPU por polling y sin actuar mientras PM sigue activo.
- **Aspectos a probar y comparar:**
  1. **Binario único:** Modo normal y modo `--guard`.
  2. **Comparación `/SWAPRUN:CD` vs. Copia a `%TEMP%`:** Verificar en la cabecera PE el flag `IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP` (`0x0400`) y contrastarlo con la estrategia de auto-copia del runtime a `%TEMP%`.
  3. **Liberación del directorio de trabajo (`CWD`):** Llamada inmediata a `SetCurrentDirectoryW` hacia `%TEMP%` para liberar cualquier handle sobre el directorio de origen.
  4. **Espera bloqueante (0% CPU):** Uso de `WaitForSingleObject(hPmProcess, INFINITE)` sobre el handle `SYNCHRONIZE` del proceso PM.
  5. **Manifiesto compartido en `%TEMP%` y exclusión mutua:** Escritura atómica (`MoveFileExW` con `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`) y Mutex de sesión (`Local\PM_Session_Mutex_<ID>`), verificando manejo de `WAIT_ABANDONED`.
  6. **Criterios de aceptación:**
     - Si PM cierra normalmente, PMG termina sin ejecutar limpieza de emergencia.
     - Si PM sigue activo, PMG permanece en espera bloqueante (0% CPU) y no limpia.
     - Si PM cae inesperadamente, PMG despierta, adquiere el mutex, lee el manifiesto en `%TEMP%`, termina el Job Object de los procesos de prueba, restaura el estado de prueba y limpia el manifiesto sin acceder al directorio de origen.

### Fase 2 — PoC Directory Junction (`poc_fase2_junction.json`)
- **Objetivo:** Implementar y validar la estrategia de Prioridad 3 para aplicaciones ZIP sin copiar perfiles físicamente al anfitrión.
- **Secuencia:**
  1. Identificar carpeta exclusiva de prueba en `%APPDATA%\PM_PoC_JunctionApp`.
  2. Comprobar existencia; si existe, renombrarla a `.pmbkp` con `MoveFileExW`.
  3. Crear una Directory Junction (`IO_REPARSE_TAG_MOUNT_POINT`) que apunte a la carpeta de datos en el USB/origen.
  4. Verificar el destino del reparse point (`FSCTL_GET_REPARSE_POINT` / `FILE_ATTRIBUTE_REPARSE_POINT`).
  5. Ejecutar aplicación de prueba que lee/escribe a través de la ruta en `%APPDATA%`.
  6. Cerrar la aplicación.
  7. Eliminar **únicamente la Junction** mediante `RemoveDirectoryW` (verificando que los datos en el destino permanecen intactos).
  8. Restaurar `.pmbkp` a su nombre original (o dejar ausente si originalmente no existía).
  9. Verificar que el anfitrión quedó idéntico a su estado inicial.
- **Escenarios:**
  - Carpeta original existente vs. inexistente.
  - USB NTFS y USB exFAT (si está disponible).
  - Cuenta estándar de Windows (sin elevación).
  - Desconexión física controlada (**deteniéndose antes para pedir confirmación expresa al usuario**).

### Fase 3 — PoC Registro de Windows (`poc_fase3_registro.json`)
- **Objetivo:** Validar el respaldo y restauración de claves HKCU exclusivamente de prueba (`HKCU\Software\PM_PoC_RegTest_<UUID>`) mediante `RegCopyTreeW`, `RegDeleteTreeW` y APIs nativas de consulta/enumeración.
- **Escenarios:**
  1. Clave originalmente inexistente.
  2. Clave existente con valores de distintos tipos (`REG_SZ`, `REG_DWORD`, `REG_BINARY`, `REG_MULTI_SZ`) y subclaves anidadas.
  3. Restauración después de cierre normal (limpiando primero la clave modificada con `RegDeleteTreeW` antes de restaurar con `RegCopyTreeW` para evitar que queden valores residuales por *merge*, y eliminando el respaldo solo tras verificar la restauración).
  4. Restauración ejecutada por PMGuard después de la caída abrupta de PM.
  5. Manejo seguro ante error simulado durante la restauración (conservando el respaldo intacto sin borrarlo).

### Fase 4 — PoC Aprendizaje Ligero (`poc_fase4_aprendizaje.json`)
- **Objetivo:** Validar el mecanismo mínimo de observación acotada sobre una aplicación de prueba sintética:
  1. Extraer metadatos PE (`CompanyName`, `ProductName`, nombre del ejecutable).
  2. Construir lista de ubicaciones candidatas acotadas (`%APPDATA%`, `%LOCALAPPDATA%`, `HKCU\Software\...`).
  3. Registrar estado inicial (baseline).
  4. Respaldar ubicaciones exclusivas que se vayan a intervenir.
  5. Ejecutar aplicación de prueba sintética.
  6. Comparar estado al cerrar.
  7. Generar receta (con rutas relativas) excluyendo cambios ambiguos o no atribuibles.
  8. Restaurar el anfitrión a su estado original y verificar ausencia de residuos.

### Fase 5 — Integración (`poc_fase5_integracion.json`)
- **Objetivo:** Integrar los componentes aprobados (PM Core + PMGuard + Manifiesto TEMP + Job Objects + Junctions + Registro + Aprendizaje + Rutinas de limpieza) y validar los tres modos de cierre:
  1. **Normal:** Cierre cooperativo, persistencia en destino portable y limpieza verificada.
  2. **Rápido:** Cierre con espera reducida, terminación si excede el plazo y restauración completa del anfitrión.
  3. **Crítico:** Terminación inmediata por Job Object y restauración prioritaria (incluyendo prueba del registro de emergencia `%TEMP%\pmlog` sin extensión cuando se simula un residuo/respaldo que no puede restaurarse con seguridad, verificando que los respaldos se conservan intactos).
