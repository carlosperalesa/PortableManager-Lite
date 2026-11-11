# Guía de Compilación Nativa C++20 — PortableManager Lite

## 1. Toolchain Oficial (LLVM-MinGW Portable)

El proyecto utiliza **LLVM-MinGW `20261104` (Clang 23.1.5, UCRT, Windows x86_64)** alojado de forma 100% portable en:

```text
<raiz_del_proyecto>/tools/toolchains/llvm-mingw/
```

- **Compilador C++20:** `tools/toolchains/llvm-mingw/bin/clang++.exe`
- **Enlazador:** `tools/toolchains/llvm-mingw/bin/ld.lld.exe`
- **Inspector PE/COFF:** `tools/toolchains/llvm-mingw/bin/llvm-readobj.exe`
- **SHA-256 del paquete original (`llvm-mingw-20261104-ucrt-x86_64.zip`):** `552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f`

No requiere instalar Visual Studio, ni modificar el registro de Windows, ni alterar el `PATH` global.

---

## 2. Opciones de Compilación C++20 Aplicadas

| Parámetro | Flag en `clang++.exe` | Propósito |
|---|---|---|
| Estándar C++ | `-std=c++20` | Conformidad con C++20 (`__cplusplus = 202002L`). |
| Advertencias | `-Wall -Wextra -Werror` | Máximo rigor: cualquier advertencia detiene la compilación. |
| Unicode | `-municode -DUNICODE -D_UNICODE` | Punto de entrada `wmain` y APIs Win32 Unicode (`W`). |
| Runtime estático del toolchain | `-static` | Enlaza estáticamente `libc++`, `libunwind` y `libwinpthread` (0 DLLs externas del toolchain). Enlaza dinámicamente con las DLLs del sistema Windows y los API sets `api-ms-win-crt-*.dll` (`ucrtbase.dll` del SO). *(Nota: no equivale a MSVC `/MT`, el cual incrusta `libucrt.lib`).* |
| Protección secundaria PE | `"-Wl,-Xlink=-swaprun:cd"` | Activa `IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP` (`0x0400`) en la cabecera PE (`Characteristics = 0x422`). |

---

## 3. Preparación y Ejecución del Build Reproducible

Desde la raíz del repositorio en PowerShell (primero descarga y verifica el toolchain si aún no existe):

```powershell
./tools/bootstrap-toolchain.ps1
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "d:\Data\build.ps1"
```

También puede utilizarse CMake (si está disponible) con el archivo de toolchain incluido:

```powershell
cmake -S d:\Data -B d:\Data\build -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE="d:\Data\cmake\llvm-mingw-toolchain.cmake"
```


## 4. Entorno Codex Cloud y CI

Para preparar el compilador Linux portable y compilar de forma cruzada Windows x64, consulta [CODEX_CLOUD.md](CODEX_CLOUD.md). Las pruebas de ejecución de los binarios Win32 se realizan en el runner Windows de GitHub Actions, no en Linux.
