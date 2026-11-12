# Entorno de desarrollo cloud — PortableManager Lite

Este documento prepara un entorno reproducible para continuar el desarrollo sin depender del PC local. No inicia fases por sí mismo ni modifica decisiones arquitectónicas. La Fase 3 fue autorizada por el propietario y su implementación está pendiente de validación Windows; Fase 4 no está autorizada.

## Entornos y responsabilidades

- **Codex Cloud (Linux):** instala LLVM-MinGW portable, compila de forma cruzada para Windows x64 e inspecciona el ejecutable PE. No ejecuta los binarios Win32.
- **GitHub Actions (Windows):** descarga el mismo LLVM-MinGW portable, compila con `build.ps1` y ejecuta las suites de regresión de Fase 0B, Fase 1 y Fase 2, más la nueva suite de Fase 3.
- **GitHub:** conserva fuentes, documentación, scripts e informes. El toolchain y los binarios generados permanecen fuera del repositorio.

## Toolchain fijado

El archivo `tools/toolchain.lock.json` fija LLVM-MinGW `20261104`, Clang `23.1.5`, UCRT y Windows x64. Los scripts consultan el release oficial de `mstorsjo/llvm-mingw`, comparan el digest SHA-256 de GitHub Releases con el valor fijado y calculan el SHA-256 del archivo descargado antes de extraerlo.

Los directorios de instalación son:

- Windows: `tools/toolchains/llvm-mingw/`.
- Linux/Codex Cloud: `tools/toolchains/llvm-mingw-linux/`.

Ambos están excluidos de Git mediante `.gitignore`.

## Preparar Codex Cloud

1. Conecta el repositorio privado `carlosperalesa/PortableManager-Lite` en Codex.
2. Crea un entorno basado en Ubuntu 22.04 x86_64.
3. Como comando de preparación del entorno utiliza:

   ```bash
   bash tools/bootstrap-codex-cloud.sh
   ```

4. El script requiere `curl`, `jq`, `tar`, `xz`, `sha256sum`, CMake y Ninja. En Ubuntu instala únicamente las herramientas que falten mediante `apt-get`; no altera el proyecto ni el sistema Windows del usuario.
5. En una tarea de Codex, para compilar e inspeccionar el PE ejecuta:

   ```bash
   bash tools/build-codex-cloud.sh
   ```

El resultado `CROSS_COMPILE_PASSED` solo demuestra compilación cruzada e inspección PE. No equivale a ejecutar las pruebas en Windows.

## Preparar Windows local o runner

Desde la raíz del repositorio, PowerShell 5.1 o posterior:

```powershell
./tools/bootstrap-toolchain.ps1
./build.ps1 -ReportJsonPath reports/ci_windows_native.json
```

El bootstrap no requiere administrador y no modifica `PATH`, Registro ni variables globales. Usa `%TEMP%` para el archivo descargado, verifica el digest oficial y extrae el compilador dentro del repositorio.

## Integración continua

El workflow `.github/workflows/portablemanager-ci.yml` ejecuta dos trabajos:

1. `cross-compile-linux`: compilación cruzada en `ubuntu-22.04` e inspección de arquitectura AMD64, `SWAPRUN:CD` y ausencia de DLL externas del toolchain.
2. `native-windows-tests`: compilación y ejecución en `windows-2022` de las suites de Fase 0B, Fase 1, Fase 2 y Fase 3. Publica `reports/ci_windows_native.json` y los binarios como artefacto temporal de CI.

Los artefactos de CI se conservan 14 días. No se sube el toolchain al repositorio.

## Límites y reglas

- No ejecutar `.exe` de Windows en Linux ni etiquetar esa ejecución como prueba superada.
- No repetir pruebas históricas sin una razón técnica; CI es una nueva regresión del estado actual del código.
- No modificar datos reales del anfitrión, HKLM ni dispositivos USB.
- Mantener todas las rutas de configuración del proyecto relativas a la raíz correspondiente; los scripts resuelven sus ubicaciones desde su propio directorio, no desde el CWD.
- No iniciar Fase 4 automáticamente. Cada nueva fase requiere autorización expresa.
- No fusionar Pull Requests ni cambiar `main` sin autorización expresa.

## Diagnóstico

- Si el digest del release no coincide con `tools/toolchain.lock.json`, detener la instalación: no actualizar el hash para hacer pasar la descarga.
- Si la compilación cruzada pasa y el trabajo Windows falla, usar el log Windows como evidencia; no sustituirlo por el resultado Linux.
- Si Codex Cloud no dispone de `apt-get` o permisos para instalar dependencias, registrar el bloqueo y utilizar un entorno Ubuntu 22.04 con CMake, Ninja, curl, jq y xz ya disponibles.

## Validación de la entrega de Fase 3

No reinstalar el toolchain si ya está disponible. En Windows, con LLVM-MinGW
portable existente, ejecutar desde la raíz:

```powershell
./build.ps1 -ReportJsonPath reports/rebuild_guard_windows_20261112_run01.json
```

Elegir una ruta nueva si el informe ya existe. El script compila y ejecuta las
cuatro suites; el detalle de Fase 3 se guarda con timestamp en `build/phase3_windows_*.json`.
El workflow de CI también recoge esas evidencias en sus artefactos. Alternativa
con CMake configurado para Windows: `ctest --test-dir build --output-on-failure`.
No ejecutar CTest sobre los PE desde Linux ni atribuirles un resultado `passed`.
