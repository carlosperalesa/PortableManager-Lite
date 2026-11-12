# Informe de Limpieza, Organización y Migración a GitHub — PortableManager Lite

**Fecha de migración:** 11 de noviembre de 2026  
**Estado:** `passed` (Publicado y verificado en repositorio privado de GitHub)

---

## 1. Datos de Publicación en GitHub

- **URL del repositorio:** `https://github.com/carlosperalesa/PortableManager-Lite`
- **Visibilidad verificada:** `private` (`private = true`)
- **Rama publicada:** `main`
- **Commit inicial publicado:** `283a86073060ee8cdb397976ce5bbed1f18a6388` (`Initial import - PortableManager Lite`)
- **Identidad Git configurada exclusivamente a nivel local (`.git/config`):**
  - `user.name`: `Carlos Perales Ayala`
  - `user.email`: `carlosperalesa@gmail.com`
  - *(No se leyó ni modificó la configuración global de Git).*

---

## 2. Respaldo Local Previo

Antes de inicializar el repositorio y organizar las carpetas de fase, se creó un respaldo local externo fuera del directorio de trabajo:
- **Ubicación del respaldo:** `C:\Users\cperales\PortableManager_Lite_Backup_20261111`
- **Estado:** Conservado intacto en disco tras el `push`.

---

## 3. Organización Documental por Fases y `AGENTS.md`

- Se verificó y ubicó [`AGENTS.md`](AGENTS.md) íntegramente en la raíz del proyecto (`18,436 bytes`), conservando también [`docs/AGENTS.md`](docs/AGENTS.md).
- Se aplicó la estructura obligatoria de dos dígitos en la raíz (`fase01/` a `fase08/`):
  - [`fase01/`](fase01): Documentación e informe JSON de Fase 1 (`README_FASE01.md`, `IMPLEMENTACION_FASE_1.md`, `rebuild_core.json`).
  - [`fase02/`](fase02): Documentación, auditoría final, informes JSON (`rebuild_runtime.json`, `rebuild_runtime_audit.json`) y ejemplos (`ejemplo_session_manifest.ini`, `ejemplo_uso_runtime.cpp`) de Fase 2. La antigua carpeta `fase2/` fue migrada sin pérdida de archivos.
  - [`fase03/`](fase03) a [`fase08/`](fase08): Creadas con sus `README_FASE03.md` .. `README_FASE08.md` en estado `not_run`.

---

## 4. Archivos Conservados vs. Archivos Excluidos (`.gitignore`)

### Archivos conservados y publicados en GitHub
- [`AGENTS.md`](AGENTS.md) y toda la documentación vigente de la raíz (`README.md`, `IMPLEMENTACION_PM_LITE.md`, `IMPLEMENTACION_FASE_0.md`, `EVALUACION_TOOLCHAIN_PORTABLE.md`, `DECISIONES_TECNICAS.md`, `PLAN_DE_PRUEBAS.md`, `RESULTADOS_RECONSTRUCCION.md`, `ARQUITECTURA_NATIVE.md`, etc.).
- Documentación funcional y de arquitectura en [`PMDocs/`](PMDocs).
- Código fuente C++20 nativo y antecedentes C# en [`src/`](src), [`tests/`](tests) y [`tools/toolchain_probe/probe.cpp`](tools/toolchain_probe/probe.cpp).
- Scripts y archivos de compilación reproducibles ([`build.ps1`](build.ps1), [`build_msvc.ps1`](build_msvc.ps1), [`CMakeLists.txt`](CMakeLists.txt), [`cmake/llvm-mingw-toolchain.cmake`](cmake/llvm-mingw-toolchain.cmake)).
- Todos los informes JSON de resultados, PoCs y evidencias de fases en [`reports/`](reports), [`fase01/`](fase01), [`fase02/`](fase02) y [`PM_Lite_FinalValidation/reports/`](PM_Lite_FinalValidation/reports).

### Archivos excluidos mediante [`.gitignore`](.gitignore) (conservados localmente en `d:\Data`)
- Directorio de salida `build/` y ejecutables compilados (`*.exe`, incluyendo `build/*.exe` y `tools/toolchain_probe/probe.exe`).
- Toolchain portable descargado en `tools/toolchains/llvm-mingw/`.
- Instaladores y paquetes de prueba (`FirefoxPortable_160.0_English.paf.exe`, `putty.zip`, `VSCode-win32-x64-1.146.0.zip`).
- Archivos y notas personales en la raíz (`1 firefox busqueda.txt`, `2 firefox purga.txt`, `vscode other.txt`, `vscode PS Mover local a portable y purgar datos locales.txt`, `Windows-FY27-CONS-C2-Wallpaper-EverydayProductivity.png`).
- Cachés `__pycache__/`, `*.pyc` y archivos `.signal`.
- Los 5 volcados crudos de inventario completo del sistema de archivos del anfitrión/USB (`d1_host_before.json`, `d1_host_after.json`, `d3_host_before_metadata.json`, `d3_host_after_metadata.json`, `usb_final_inventory.json`).

---

## 5. Resultado de la Auditoría de Seguridad

- **Secretos, tokens o claves privadas:** `0` encontrados.
- **Datos personales del anfitrión:** Los volcados masivos con el listado completo de archivos del perfil de usuario (`d3_host_before/after_metadata.json`, ~46.4 MB cada uno) fueron identificados y excluidos en `.gitignore` antes del primer commit.

---

## 6. Problemas Encontrados y Resolución

1. **Selección de cuenta en Windows Credential Manager:** Existían dos cuentas de GitHub almacenadas en el Administrador de Credenciales de Windows (`102628412` y `carlosperalesa`), lo que provocaba que Git Credential Manager solicitara un diálogo interactivo de selección de cuenta. Se resolvió configurando exclusivamente en `.git/config` local `credential.https://github.com.username = carlosperalesa`.
2. **Volcados masivos de metadatos del anfitrión:** Se detectaron dos archivos de ~46.4 MB (`d3_host_before_metadata.json` y `d3_host_after_metadata.json`) y dos de ~4.5 MB (`d1_host_before/after.json`) con rutas completas del perfil del usuario; se excluyeron mediante reglas específicas en `.gitignore` sin afectar a ningún informe JSON de resultados de fases o PoCs.
