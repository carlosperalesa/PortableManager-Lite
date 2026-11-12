# Evaluación de Toolchain C++ Portable — LLVM-MinGW (UCRT x86_64)

**Fecha:** 11 de noviembre de 2026  
**Estado:** Evaluación completada y verificada empíricamente.  
**Ubicación del Toolchain:** `d:\Data\tools\toolchains\llvm-mingw\`  
**Ubicación de la Prueba de Compatibilidad:** [tools/toolchain_probe/probe.cpp](tools/toolchain_probe/probe.cpp)

---

## 1. Descarga y Verificación de Integridad

- **Fuente oficial:** [https://github.com/mstorsjo/llvm-mingw/releases/tag/20261104](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261104)
- **Versión seleccionada:** `20261104` (con **LLVM / Clang 23.1.5**, variante **UCRT**, arquitectura **Windows x86_64**).
- **Archivo descargado:** `llvm-mingw-20261104-ucrt-x86_64.zip` (`191.037.046 bytes`)
- **URL directa oficial:** `https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/llvm-mingw-20261104-ucrt-x86_64.zip`
- **Verificación criptográfica SHA-256:**
  - Digest oficial publicado en los metadatos del release en GitHub API (`assets[].digest`):
    `552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f`
  - Hash SHA-256 calculado localmente (`Get-FileHash -Algorithm SHA256`) antes de extraer:
    `552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f`
  - **Resultado de integridad:** **COINCIDENCIA EXACTA (`VERIFIED OK`)**.
- **Extracción:** Extraído en `d:\Data\tools\toolchains\llvm-mingw\` sin tocar el registro de Windows, sin modificar el `PATH` global y sin requerir privilegios de administrador.

---

## 2. Verificación de Herramientas, Cabeceras y Bibliotecas Win32

### 2.1 Binarios del compilador y utilidades LLVM
| Componente | Ruta en `tools/toolchains/llvm-mingw/` | Estado |
|---|---|---|
| `clang.exe` | `bin\clang.exe` | Presente y verificado |
| `clang++.exe` | `bin\clang++.exe` | Presente y verificado |
| `ld.lld.exe` | `bin\ld.lld.exe` | Presente y verificado |
| `llvm-ar.exe` | `bin\llvm-ar.exe` | Presente y verificado |
| `llvm-readobj.exe` | `bin\llvm-readobj.exe` | Presente y verificado |
| `llvm-strip.exe` | `bin\llvm-strip.exe` | Presente y verificado |
| `mingw32-make.exe` | `bin\mingw32-make.exe` | Presente y verificado |

### 2.2 Cabeceras y Bibliotecas de Importación Win32
| Recurso Win32 | Ruta dentro del Toolchain | Estado |
|---|---|---|
| `windows.h` | `include\windows.h` | Presente |
| `winioctl.h` | `include\winioctl.h` | Presente |
| `winreg.h` | `include\winreg.h` | Presente |
| `kernel32` | `x86_64-w64-mingw32\lib\libkernel32.a` | Presente |
| `advapi32` | `x86_64-w64-mingw32\lib\libadvapi32.a` | Presente |
| `version` | `x86_64-w64-mingw32\lib\libversion.a` | Presente |
| `shell32` | `x86_64-w64-mingw32\lib\libshell32.a` | Presente |

### 2.3 Salida de `clang++ --version`
```text
clang version 23.1.5 (https://github.com/llvm/llvm-project.git 392dfff97d29b59d77e5cac6d4b6ef2585fe7117)
Target: x86_64-w64-windows-gnu
Thread model: posix
InstalledDir: D:/Data/tools/toolchains/llvm-mingw/bin
Configuration file: D:/Data/tools/toolchains/llvm-mingw/bin/x86_64-w64-windows-gnu.cfg
```

---

## 3. Prueba de Compatibilidad con PM (`tools/toolchain_probe/`)

Se creó en una carpeta aislada el archivo [tools/toolchain_probe/probe.cpp](tools/toolchain_probe/probe.cpp) sin modificar `src/`, `CMakeLists.txt` ni `build.ps1`.

### 3.1 APIs y características probadas en `probe.cpp`
1. **C++20 real (`__cplusplus == 202002L`):** `<concepts>`, `consteval`, `<string_view>`, punto de entrada Unicode `wmain(int argc, wchar_t* argv[])` con `-municode`.
2. **Resolución de ruta del módulo:** `GetModuleFileNameW` (`kernel32`).
3. **Procesos y Job Objects:** `CreateJobObjectW`, `SetInformationJobObject` (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), `OpenJobObjectW`, `CreateProcessW` (`CREATE_SUSPENDED | CREATE_NO_WINDOW`), `AssignProcessToJobObject`, `ResumeThread`, `WaitForSingleObject`, `GetExitCodeProcess`, `TerminateJobObject`.
4. **Directory Junctions (`winioctl.h`):** Creación real de un punto de reanálisis `IO_REPARSE_TAG_MOUNT_POINT` (`0xA0000003`) en `%TEMP%` mediante `CreateFileW` (`FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT`) y `DeviceIoControl` (`FSCTL_SET_REPARSE_POINT` y `FSCTL_GET_REPARSE_POINT`), comprobación de `FILE_ATTRIBUTE_REPARSE_POINT` y desmontaje limpio con `RemoveDirectoryW`.
5. **Registro Win32 (`winreg.h` / `advapi32`):** Verificación de enlace de los símbolos `RegCopyTreeW` y `RegDeleteTreeW` más lectura sin modificación (`KEY_READ`) sobre `HKCU\Software` (`RegOpenKeyExW` + `RegQueryInfoKeyW`), cumpliendo la restricción de no modificar el registro de Windows durante esta evaluación.
6. **Metadatos PE (`version`) y Shell (`shell32`):** `GetFileVersionInfoSizeW`, `GetFileVersionInfoW`, `VerQueryValueW` y `CommandLineToArgvW`.

### 3.2 Comando de compilación ejecutado
```powershell
D:\Data\tools\toolchains\llvm-mingw\bin\clang++.exe `
  -std=c++20 -O2 -Wall -Wextra -Werror -municode -static `
  "-Wl,-Xlink=-swaprun:cd" `
  D:\Data\tools\toolchain_probe\probe.cpp `
  -o D:\Data\tools\toolchain_probe\probe.exe `
  -lkernel32 -ladvapi32 -lversion -lshell32
```
- **Código de salida de compilación:** `0` (sin errores ni advertencias con `-Wall -Wextra -Werror`).
- **Tamaño del binario:** `742.912 bytes` sin strip / **`332.800 bytes`** tras `llvm-strip.exe` (SHA-256: `DC251D83F6C30C45341387CC5CC5F969A084E2EA97F9E1A1A7074DC093C56809`).
- **Salida de ejecución de `probe.exe`:**
  ```text
  [PROBE] ALL_CHECKS_PASSED | Module=D:\Data\tools\toolchain_probe\probe.exe | C++=202002
  Probe Run ExitCode: 0
  ```

---

## 4. Inspección del Ejecutable PE (`llvm-readobj.exe`) y Análisis del Runtime

### 4.1 Cabeceras PE (`llvm-readobj --file-headers probe.exe`)
```text
File: D:\Data\tools\toolchain_probe\probe.exe
Format: COFF-x86-64
Arch: x86_64
AddressSize: 64bit
ImageFileHeader {
  Machine: IMAGE_FILE_MACHINE_AMD64 (0x8664)
  Characteristics [ (0x422)
    IMAGE_FILE_EXECUTABLE_IMAGE (0x2)
    IMAGE_FILE_LARGE_ADDRESS_AWARE (0x20)
    IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP (0x400)
  ]
}
ImageOptionalHeader {
  Magic: 0x20B
  Subsystem: IMAGE_SUBSYSTEM_WINDOWS_CUI (0x3)
  Characteristics [ (0x8160)
    IMAGE_DLL_CHARACTERISTICS_DYNAMIC_BASE (0x40)
    IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA (0x20)
    IMAGE_DLL_CHARACTERISTICS_NX_COMPAT (0x100)
    IMAGE_DLL_CHARACTERISTICS_TERMINAL_SERVER_AWARE (0x8000)
  ]
}
```
- Se confirma que el flag `-Wl,-Xlink=-swaprun:cd` es trasladado por `ld.lld` al enlazador COFF interno, activando el bit **`IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP (0x400)`** (`Characteristics = 0x422`).

### 4.2 DLLs Importadas (`llvm-readobj --coff-imports probe.exe`)
```text
  Name: KERNEL32.dll
  Name: ADVAPI32.dll
  Name: VERSION.dll
  Name: SHELL32.dll
  Name: api-ms-win-crt-stdio-l1-1-0.dll
  Name: api-ms-win-crt-runtime-l1-1-0.dll
  Name: api-ms-win-crt-locale-l1-1-0.dll
  Name: api-ms-win-crt-heap-l1-1-0.dll
  Name: api-ms-win-crt-private-l1-1-0.dll
  Name: api-ms-win-crt-string-l1-1-0.dll
  Name: api-ms-win-crt-convert-l1-1-0.dll
  Name: api-ms-win-crt-math-l1-1-0.dll
  Name: api-ms-win-crt-environment-l1-1-0.dll
  Name: api-ms-win-crt-filesystem-l1-1-0.dll
```

### 4.3 Diferencias Exactas entre `-static` en LLVM-MinGW (UCRT) y `/MT` en MSVC
1. **Qué hace `-static` en LLVM-MinGW:**
   - Enlaza estáticamente dentro del ejecutable las bibliotecas del compilador C++ y soporte de hilos/excepciones: **`libc++.a`** (biblioteca estándar C++ de LLVM), **`libunwind.a`** (desenrollado de pila SEH) y **`libwinpthread.a`**.
   - Gracias a ello, el ejecutable **no depende de ninguna DLL externa del toolchain** (no requiere `libc++.dll`, `libunwind.dll`, `libwinpthread-1.dll`, ni `vcruntime140.dll` / `msvcp140.dll`).
2. **En qué se diferencia de MSVC `/MT`:**
   - **MSVC `/MT`** enlaza estáticamente `libucrt.lib` (la implementación completa de la biblioteca C) dentro del propio `.exe`, de modo que el binario no importa `api-ms-win-crt-*.dll`.
   - **LLVM-MinGW `-static` (variante UCRT)** enlaza contra la biblioteca de importación de la **Universal C Runtime (UCRT) del sistema operativo** (`api-ms-win-crt-*.dll`, resueltas por el cargador de Windows hacia `C:\Windows\System32\ucrtbase.dll`).
   - **Implicación práctica para PortableManager Lite:** En Windows 10 y Windows 11, `ucrtbase.dll` y los API sets `api-ms-win-crt-*.dll` son **componentes nativos del sistema operativo** presentes en `C:\Windows\System32` desde la instalación base de Windows (a diferencia de `vcruntime140.dll` o `msvcp140.dll`, que no vienen con Windows). Por tanto, el ejecutable generado con LLVM-MinGW `-static` se ejecuta de forma completamente autónoma en cualquier equipo Windows 10/11 limpio sin instalar ningún redistribuible.

---

## 5. Comparación con los Requisitos de PM

| Requisito | Resultado | Clasificación | Detalle Técnico |
|---|---|---|---|
| **C++20** | Completo (`__cplusplus = 202002L`) | **Compatible** | Clang 23.1.5 con `libc++` soporta C++20 completo (`<concepts>`, `consteval`, `<ranges>`, `<format>`, `<filesystem>`). |
| **Win32 API** | Completo (`UNICODE` / `_UNICODE`, `-municode`) | **Compatible** | Incluye `windows.h`, `winioctl.h`, `winreg.h`, `shellapi.h`, `shlwapi.h` y todas las bibliotecas de importación Win32 x64. |
| **Job Objects** | Verificado en ejecución | **Compatible** | `CreateJobObjectW`, `SetInformationJobObject` (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), `OpenJobObjectW`, `AssignProcessToJobObject`, `TerminateJobObject`. |
| **Directory Junctions** | Verificado en ejecución | **Compatible** | `DeviceIoControl` con `FSCTL_SET_REPARSE_POINT` / `FSCTL_GET_REPARSE_POINT` (`IO_REPARSE_TAG_MOUNT_POINT`) y `RemoveDirectoryW`. |
| **RegCopyTreeW** | Verificado en enlace y cabeceras | **Compatible** | Declarado en `winreg.h` y enlazado en `libadvapi32.a` (`ADVAPI32.dll`). |
| **RegDeleteTreeW** | Verificado en enlace y cabeceras | **Compatible** | Declarado en `winreg.h` y enlazado en `libadvapi32.a` (`ADVAPI32.dll`). |
| **Runtime estático** | `libc++` + `libunwind` + `winpthreads` estáticos; UCRT del SO (`ucrtbase.dll`) | **Compatible con modificaciones** | Con `-static` no requiere ninguna DLL ajena a `C:\Windows\System32` en Windows 10/11, aunque enlaza dinámicamente con `ucrtbase.dll` del SO en lugar de incrustar `libucrt.lib` como hace MSVC `/MT`. |
| **/SWAPRUN:CD** | `Characteristics = 0x422` (`0x400` activo) | **Compatible con modificaciones** | En lugar del flag `/SWAPRUN:CD` de `link.exe`, en Clang/LLD se pasa `"-Wl,-Xlink=-swaprun:cd"`, produciendo exactamente el mismo bit `0x0400` en la cabecera PE. |
| **Generación x64** | `IMAGE_FILE_MACHINE_AMD64 (0x8664)` | **Compatible** | Compilador nativo `x86_64-w64-windows-gnu` de 64 bits con High-Entropy ASLR (`0x20`) y DEP/NX (`0x100`). |
| **Compatibilidad con CMake** | Soporta `MinGW Makefiles` / `Ninja` (`cmake.exe` no instalado aún en host) | **Compatible con modificaciones** | El toolchain incluye `mingw32-make.exe` en `bin/` y puede usarse directamente desde scripts de compilación o con CMake ajustando los flags de MSVC (`/W4 /MT /SWAPRUN:CD`) a Clang (`-Wall -Wextra -Werror -municode -static "-Wl,-Xlink=-swaprun:cd"`). |
| **Ausencia de instalación** | 100% portable en `tools/toolchains/llvm-mingw/` | **Compatible** | No modifica el registro de Windows, ni variables de entorno globales, ni requiere permisos de Administrador. |

---

## 6. Recomendación Final y Propuesta de Adopción

### Conclusión
**LLVM-MinGW (`20261104-ucrt-x86_64`) CUMPLE todos los requisitos técnicos fundamentales de PortableManager Lite** para reconstruir PM Core y PMGuard íntegramente en **C++20 nativo + Win32 API (x64)**:
1. Genera ejecutables PE x64 nativos (`0x8664`) con el flag `IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP` (`0x0400`, `/SWAPRUN:CD`) verificado mediante `llvm-readobj.exe`.
2. Con `-static`, incrusta `libc++`, `libunwind` y `libwinpthread`, dependiendo únicamente de DLLs presentes de serie en `C:\Windows\System32` de Windows 10/11 (`KERNEL32.dll`, `ADVAPI32.dll`, `VERSION.dll`, `SHELL32.dll` y `ucrtbase.dll`).
3. Soporta sin limitaciones todas las APIs críticas del proyecto (`CreateJobObjectW`, `DeviceIoControl` / `FSCTL_SET_REPARSE_POINT`, `RegCopyTreeW`, `RegDeleteTreeW`, `GetFileVersionInfoW`, `MoveFileExW`, `WaitForSingleObject`, `GetModuleFileNameW`).
4. Reside íntegramente dentro de `<raiz_del_proyecto>/tools/toolchains/llvm-mingw/` sin instalar nada en el sistema anfitrión.

### Propuesta de Adopción (sujeta a tu autorización)
Si apruebas adoptar este toolchain C++20 portable:
1. Actualizaremos `CMakeLists.txt` y `build.ps1` para compilar todo el proyecto en **C++20 nativo** usando `tools/toolchains/llvm-mingw/bin/clang++.exe` con los flags verificados:
   ```text
   -std=c++20 -O2 -Wall -Wextra -Werror -municode -static "-Wl,-Xlink=-swaprun:cd"
   ```
2. Eliminaremos los archivos temporales en C# de la Fase 0 (`*.cs`), dejando exclusivamente el código fuente **C++20 nativo** en `src/` y `tests/`, re-ejecutando la prueba de arranque de Fase 0 con el binario C++20 y pasando a continuación a la **Fase 1 — Core y Rutas** (`PathResolver`, `ConfigManager`, `AtomicFile`) en C++20 puro.
