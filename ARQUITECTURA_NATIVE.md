# Arquitectura Nativa C++20 / Win32 — PortableManager Lite

**Versión:** 1.0 (Reconstrucción Nativa)  
**Fecha:** 11 de noviembre de 2026

---

## 1. Estructura Modular del Proyecto

El código fuente nativo se organiza en nueve módulos bajo [`src/`](file:///d:/Data/src), más la suite de pruebas independientes en [`tests/`](file:///d:/Data/tests) y documentación de compilación en [`docs/`](file:///d:/Data/docs):

```text
d:\Data\
├── CMakeLists.txt
├── build_msvc.ps1
├── src\
│   ├── core\           # Punto de entrada (wmain), PathResolver, ConfigManager, AtomicFile
│   ├── runtime\        # SessionManager y ManifestManager (%TEMP%\PortableManager\Session\<SessionID>\session.ini)
│   ├── guard\          # Modo --guard <SessionID>, espera bloqueante (WaitForSingleObject), Mutex de sesión
│   ├── jobs\           # JobManager (CreateJobObjectW, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, TerminateJobObject)
│   ├── junctions\      # JunctionManager (FSCTL_SET_REPARSE_POINT, IO_REPARSE_TAG_MOUNT_POINT, RemoveDirectoryW)
│   ├── registry\       # RegistryManager (respaldo .reg, RegDeleteTreeW, verificación)
│   ├── learning\       # LearningEngine (GetFileVersionInfoW, VerQueryValueW, candidatos acotados, recetas INI)
│   ├── applications\   # Lanzamiento PAF/ZIP, evaluación de Prioridades 1/2/3, clasificación por colores
│   └── cleanup\        # CleanupEngine (NORMAL, FAST, CRITICAL y generación de %TEMP%\pmlog)
├── tests\
│   └── test_phase0_smoke.cpp
└── docs\
    └── README_COMPILACION.md
```

---

## 2. Responsabilidades por Subsistema

1. **`src/core/` (Fase 0 y Fase 1):**
   - `GetModuleFileNameW` para resolver siempre la raíz real de `pm.exe` sin depender jamás de `GetCurrentDirectoryW`.
   - `PathResolver`: conversión entre rutas relativas portables y rutas absolutas efímeras en tiempo de ejecución; expansión controlada de `%APPDATA%` y `%LOCALAPPDATA%`; bloqueo de traversal (`..`) fuera de la raíz de PM cuando no esté autorizado.
   - `AtomicFile`: escritura en archivo temporal hermano + `FlushFileBuffers` + `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.
   - `ConfigManager`: lectura/escritura UTF-8 sin BOM de `Data/config.ini`, `Data/apps.ini` y `Data/recipes/<id>.ini`, preservando claves y comentarios no modificados.

2. **`src/runtime/` (Fase 2):**
   - `SessionManager` y `ManifestManager`: administración de `%TEMP%\PortableManager\Session\<SessionID>\session.ini`.
   - Registro previo obligatorio (*write-ahead*) antes de cualquier modificación en el anfitrión y actualización atómica tras completar cada paso.

3. **`src/jobs/` y `src/guard/` (Fase 3):**
   - `JobManager`: objetos Job nombrados por sesión/aplicación con `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
   - `PMGuard`: mismo binario `pm.exe` copiado a `%TEMP%`, con CWD liberado hacia `%TEMP%` (`SetCurrentDirectoryW`), esperando el handle del proceso PM mediante `WaitForSingleObject(..., INFINITE)` (0% CPU) y coordinado mediante Mutex nombrado (`Local\PM_Session_Mutex_<SessionID>`).

4. **`src/junctions/` (Fase 4):**
   - `JunctionManager`: renombrado `.pmbkp`, creación y lectura de puntos de reanálisis `IO_REPARSE_TAG_MOUNT_POINT` (`0xA0000003`) mediante `DeviceIoControl`, y eliminación segura del reparse point con `RemoveDirectoryW` tras validar `FILE_ATTRIBUTE_REPARSE_POINT` y el destino esperado.

5. **`src/registry/` (Fase 5):**
   - `RegistryManager`: respaldo de subárboles exclusivos de HKCU mediante `reg.exe export` en la sesión temporal, validación de alcance e integridad, borrado previo con `RegDeleteTreeW` e importación tras cerrar los Jobs. El respaldo se retira tras verificar la restauración y persistir el resultado. [Detalles](fase05/README_FASE05.md).

6. **`src/learning/` (Fase 6):**
   - `LearningEngine`: inspección de recursos PE (`version.dll`) y candidatos acotados a `<Company>\<Product>` en `%APPDATA%`, `%LOCALAPPDATA%` y `HKCU\Software`. La ejecución requiere atribución exclusiva previa y usa los gestores de Junctions, Registro, Jobs y PMGuard. Registra los recursos realmente afectados y genera una receta relativa con reglas de limpieza y restauración, sin cosechar ni guardar configuraciones de la aplicación. Los respaldos de sesión solo sirven para restaurar el anfitrión. [Detalles](fase06/README_FASE06.md).

7. **`src/cleanup/` (Fase 7):**
   - `CleanupEngine` comparte la limpieza de PM y PMGuard bajo el mutex de sesión. `NORMAL` solicita cierre cooperativo, `FAST` limita la espera cooperativa a 200 ms y `CRITICAL` inicia terminación inmediata. El rollback de Junctions y HKCU empieza cuando se confirma el vaciado de los Jobs. La finalización preserva respaldos pendientes y crea `%TEMP%\pmlog` solo si quedan residuos u operaciones sin completar. [Protocolo](fase07/README_FASE07.md).
