# Implementación Fase 0 y Fase 0B — Preparación del Proyecto y Adopción de LLVM-MinGW C++20

**Fecha:** 11 de noviembre de 2026  
**Estado Definitivo de Fase 0 / 0B:** `passed`  
**Informe JSON Oficial (C++20 Nativo):** [reports/fase0b_native.json](../reports/fase0b_native.json) y [PM_Lite_FinalValidation/reports/fase0b_native.json](../PM_Lite_FinalValidation/reports/fase0b_native.json)
**Evaluación del Toolchain:** [EVALUACION_TOOLCHAIN_PORTABLE.md](EVALUACION_TOOLCHAIN_PORTABLE.md)

---

## 1. Trazabilidad y Resolución del Estado `blocked` → `passed`

Para evitar cualquier contradicción documental entre fases intermedias, se deja constancia cronológica exacta del proceso seguido en la Fase 0:

1. **Fase 0 Inicial (Detección de ausencia de compilador C++ en el sistema → `blocked`):**
   - Al inspeccionar inicialmente el equipo anfitrión, se comprobó que ni Visual Studio (`cl.exe`), ni el Windows SDK (`Windows.h`), ni CMake estaban instalados en el sistema (`vswhere.exe` devolvió `[]`).
   - En cumplimiento estricto de las Secciones 4 y 5 (*"No instalar herramientas automáticamente"*, *"Detenerse si no se consigue compilar"*), el paso inicial de compilación C++ se registró como `blocked` y se creó temporalmente un antecedente provisional en C# (`rebuild_phase0.json`), cuyos archivos `.cs` e informe se conservan intactos como registro histórico (Sección 5 de Fase 0B).
2. **Evaluación de Toolchain C++ Portable ([EVALUACION_TOOLCHAIN_PORTABLE.md](EVALUACION_TOOLCHAIN_PORTABLE.md)):**
   - Se descargó desde el repositorio oficial (`https://github.com/mstorsjo/llvm-mingw/releases/tag/20261104`) el paquete portable `llvm-mingw-20261104-ucrt-x86_64.zip` (Clang 23.1.5, UCRT, Windows x86_64) en `tools/toolchains/llvm-mingw/`, verificando su hash SHA-256 (`552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f`) contra el digest oficial de GitHub Releases.
3. **Fase 0B — Adopción Oficial de LLVM-MinGW y Compilación C++20 Nativa (`passed`):**
   - Se adoptó `tools/toolchains/llvm-mingw/bin/clang++.exe` como compilador oficial C++20 para el ejecutable definitivo `pm.exe` (descartando C# y MSVC para el binario final).
   - Se compiló y validó `build/pm.exe` desde [`src/core/main.cpp`](../src/core/main.cpp) y [`src/guard/guard_entry.cpp`](../src/guard/guard_entry.cpp), superando todas las verificaciones de [`tests/test_phase0_smoke.cpp`](../tests/test_phase0_smoke.cpp) y de `llvm-readobj.exe`.

---

## 2. Configuración del Compilador y Características PE Verificadas

### 2.1 Flags de Compilación C++20 en [`build.ps1`](../build.ps1) y [`CMakeLists.txt`](../CMakeLists.txt)
```text
-std=c++20 -O2 -Wall -Wextra -Werror -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -municode -static "-Wl,-Xlink=-swaprun:cd"
```
- **Resolución de rutas en `build.ps1`:** Todas las rutas se calculan de forma relativa a la ubicación del script (`$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path`), sin depender del directorio de trabajo actual (`CWD`) ni modificar el `PATH` global de Windows.

### 2.2 Inspección PE de `build/pm.exe` mediante `llvm-readobj.exe`
- **Ruta compilada:** `d:\Data\build\pm.exe`
- **Tamaño:** `736.768 bytes`
- **SHA-256:** `EB0A5160DF10D47B8EA0C940F584595DCBE6EBD1589D59139B14C2299637BBAF`
- **Arquitectura (`Machine`):** `IMAGE_FILE_MACHINE_AMD64 (0x8664)`
- **Características (`Characteristics`):** `0x422`:
  - `IMAGE_FILE_EXECUTABLE_IMAGE (0x2)`
  - `IMAGE_FILE_LARGE_ADDRESS_AWARE (0x20)`
  - `IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP (0x400)` (`/SWAPRUN:CD` activo)
- **DLLs importadas (`--coff-imports`):**
  - `KERNEL32.dll`
  - `api-ms-win-crt-stdio-l1-1-0.dll`
  - `api-ms-win-crt-runtime-l1-1-0.dll`
  - `api-ms-win-crt-locale-l1-1-0.dll`
  - `api-ms-win-crt-heap-l1-1-0.dll`
  - `api-ms-win-crt-private-l1-1-0.dll`
  - `api-ms-win-crt-string-l1-1-0.dll`
  - `api-ms-win-crt-convert-l1-1-0.dll`
  - `api-ms-win-crt-math-l1-1-0.dll`
  - `api-ms-win-crt-environment-l1-1-0.dll`
  - `api-ms-win-crt-filesystem-l1-1-0.dll`
- **Nota técnica sobre el runtime (`-static` vs. MSVC `/MT`):**
  - `-static` en LLVM-MinGW enlaza estáticamente dentro de `pm.exe` las bibliotecas del compilador (`libc++.a`, `libunwind.a`, `libwinpthread.a`), logrando **cero dependencias de DLL externas del toolchain** (`external_toolchain_dlls = []`).
  - No equivale a MSVC `/MT`: mientras MSVC `/MT` incrusta estáticamente `libucrt.lib` dentro del ejecutable, LLVM-MinGW UCRT enlaza contra los API sets `api-ms-win-crt-*.dll` resueltos hacia `C:\Windows\System32\ucrtbase.dll` (componente nativo incluido de serie en toda instalación de Windows 10 y Windows 11).

---

## 3. Pruebas Ejecutadas en Fase 0B (`passed`)

Ejecutado [`build.ps1`](../build.ps1) invocándolo desde `C:\` (fuera del directorio del proyecto):
1. **Compilación limpia (`clean_build_succeeded = true`):** `pm.exe` y `test_phase0_smoke.exe` compilados desde cero con `-Wall -Wextra -Werror` sin advertencias.
2. **Arranque y salida normal (`normal_start_and_exit_code_0 = true`):** `pm.exe` inició, imprimió su raíz resuelta por `GetModuleFileNameW` y finalizó con código `0`.
3. **Ejecución independiente del CWD (`execution_from_different_cwd_temp = true`):** Lanzado con `lpCurrentDirectory = %TEMP%`, `pm.exe --smoke-test` resolvió `Root=D:\Data\build` mediante `GetModuleFileNameW`.
4. **Invocación de `--guard` (`guard_mode_invocation_code_0 = true` y `guard_mode_missing_session_rejected = true`):** `pm.exe --guard fase0b_session_test` devolvió `0`; `pm.exe --guard` sin `SessionID` devolvió `2`.
5. **Verificación PE y de importaciones (`pe_and_imports_verified_by_readobj = true`):** Comprobado automáticamente mediante `llvm-readobj.exe`.
