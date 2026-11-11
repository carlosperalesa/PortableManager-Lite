# PortableManager Lite (PM Lite) — Núcleo Nativo C++20

**Estado actual:** Fase 0B, Fase 1 y Fase 2 (con Auditoría Final de Fase 2) completadas y aprobadas (`passed`). Esperando autorización para iniciar **Fase 3 (JobObjects y PMGuard)**.  
**Toolchain oficial:** LLVM-MinGW `20261104` (Clang `23.1.5`, UCRT, Windows `x86_64`) portable en `tools/toolchains/llvm-mingw/`.

---

## 1. Descripción del Proyecto

**PortableManager Lite (PM)** es un entorno portátil ligero en C++20 y Win32 API para Windows que administra aplicaciones portables (PAF y ZIP), sus datos, procesos y la restauración limpia del equipo anfitrión, operando desde cualquier ubicación (USB o carpeta local) mediante rutas relativas a `pm.exe` (`GetModuleFileNameW`).

---

## 2. Compilación Reproducible

El proyecto no requiere instalar Visual Studio ni modificar variables de entorno globales de Windows. El script [`build.ps1`](file:///d:/Data/build.ps1) detecta automáticamente la raíz del proyecto y utiliza el compilador portable en `tools/toolchains/llvm-mingw/bin/clang++.exe`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "d:\Data\build.ps1"
```

### Parámetros de compilación C++20 aplicados
- `-std=c++20 -O2 -Wall -Wextra -Werror`
- `-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -municode`
- `-static` (enlaza estáticamente `libc++`, `libunwind` y `libwinpthread`; importa las DLLs del sistema operativo Windows y los API sets `api-ms-win-crt-*.dll` de `ucrtbase.dll` nativo de Windows 10/11).
- `"-Wl,-Xlink=-swaprun:cd"` (activa `IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP = 0x0400` en la cabecera PE como protección secundaria junto con la copia a `%TEMP%`).

Para entornos con CMake, se incluye [`CMakeLists.txt`](file:///d:/Data/CMakeLists.txt) y el archivo de toolchain [`cmake/llvm-mingw-toolchain.cmake`](file:///d:/Data/cmake/llvm-mingw-toolchain.cmake).

---

## 3. Estructura del Proyecto y Organización Documental por Fase (`fase01/` .. `fase08/`)

Siguiendo la directiva permanente de organización documental, cada fase dispone de su carpeta propia de dos dígitos en la raíz (`fase01/` a `fase08/`):

```text
d:\Data\
├── build.ps1                         # Script reproducible de compilación y prueba C++20
├── CMakeLists.txt                    # Configuración CMake (Clang/LLVM-MinGW y MSVC)
├── cmake\
│   └── llvm-mingw-toolchain.cmake    # Toolchain file CMake para LLVM-MinGW portable
├── fase01\                           # Consolidado documental y resultados de Fase 1 (Core y Rutas)
├── fase02\                           # Consolidado documental, auditoría, JSON y ejemplos de Fase 2 (Runtime)
├── tools\
│   ├── toolchains\llvm-mingw\        # Toolchain portable LLVM-MinGW 20261104 (Clang 23.1.5 UCRT x64)
│   └── toolchain_probe\              # Prueba aislada de compatibilidad Win32 / C++20
├── src\
│   ├── core\                         # Fase 0B/1: main.cpp, PathResolver, AtomicFile, ConfigManager
│   ├── guard\                        # Entrada modo --guard (guard_entry.cpp, guard_entry.h)
│   ├── runtime\                      # Fase 2: SessionManager y ManifestManager
│   ├── jobs\                         # Fase 3: JobManager
│   ├── junctions\                    # Fase 4: JunctionManager
│   ├── registry\                     # Fase 5: RegistryManager
│   ├── learning\                     # Fase 6: LearningEngine
│   ├── cleanup\                      # Fase 7: CleanupEngine
│   └── applications\                 # Fase 8: Gestión de aplicaciones
├── tests\
│   ├── test_phase0_smoke.cpp         # Suite C++20 de prueba de arranque y salida (Fase 0B)
│   ├── test_phase1_core.cpp          # Suite C++20 de Core y Rutas (Fase 1)
│   └── test_phase2_runtime.cpp       # Suite C++20 de Runtime y Manifiesto (Fase 2, 23 escenarios)
└── reports\
    ├── fase0b_native.json            # Evidencia JSON de Fase 0B
    ├── rebuild_core.json             # Evidencia JSON de Fase 1
    ├── rebuild_runtime.json          # Evidencia JSON inicial de Fase 2 (histórico intacto)
    └── rebuild_runtime_audit.json    # Evidencia JSON de Auditoría Final de Fase 2 (23 escenarios)
```

---

## 4. Documentación Principal

- [IMPLEMENTACION_PM_LITE.md](file:///d:/Data/IMPLEMENTACION_PM_LITE.md) — Estado global de implementación por fases.
- [IMPLEMENTACION_FASE_0.md](file:///d:/Data/IMPLEMENTACION_FASE_0.md) — Informe de Fase 0 y Fase 0B (`passed`).
- [fase02/README_FASE02.md](file:///d:/Data/fase02/README_FASE02.md) — Documentación consolidada, ejemplos e informe de auditoría de Fase 2.
- [fase02/INFORME_DE_AUDITORIA.md](file:///d:/Data/fase02/INFORME_DE_AUDITORIA.md) — Informe de Auditoría Final de Fase 2.
- [EVALUACION_TOOLCHAIN_PORTABLE.md](file:///d:/Data/EVALUACION_TOOLCHAIN_PORTABLE.md) — Evaluación técnica de LLVM-MinGW.
- [DECISIONES_TECNICAS.md](file:///d:/Data/DECISIONES_TECNICAS.md) — Decisiones arquitectónicas y precedencia normativa.
- [ARQUITECTURA_NATIVE.md](file:///d:/Data/ARQUITECTURA_NATIVE.md) — Diseño modular C++20 / Win32.
- [PLAN_DE_PRUEBAS.md](file:///d:/Data/PLAN_DE_PRUEBAS.md) — Matriz de pruebas por fase.
- [RESULTADOS_RECONSTRUCCION.md](file:///d:/Data/RESULTADOS_RECONSTRUCCION.md) — Estado consolidado de las fases de reconstrucción.
- [docs/README_COMPILACION.md](file:///d:/Data/docs/README_COMPILACION.md) — Detalles del proceso de compilación e inspección PE.
