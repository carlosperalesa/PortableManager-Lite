#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
T="$ROOT/tools/toolchains/llvm-mingw-linux"
BUILD="$ROOT/build/codex-cloud"
test -x "$T/bin/clang++" || { echo "Run tools/bootstrap-codex-cloud.sh first." >&2; exit 1; }
test -x "$T/bin/llvm-readobj"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/llvm-mingw-linux-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --parallel
PML="$BUILD/pml.exe"
test -f "$PML"
HEADERS="$("$T/bin/llvm-readobj" --file-headers "$PML")"
grep -q IMAGE_FILE_MACHINE_AMD64 <<<"$HEADERS"
grep -q IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP <<<"$HEADERS"
IMPORTS="$("$T/bin/llvm-readobj" --coff-imports "$PML")"
if grep -Eiq "Name: (libc\+\+|libstdc\+\+|libunwind|libgcc|libwinpthread)" <<<"$IMPORTS"; then echo "External toolchain DLL import found." >&2; exit 1; fi
echo "CROSS_COMPILE_PASSED: Windows x64 PE built and inspected."
echo "WINDOWS_RUNTIME_TESTS_NOT_RUN: execute on windows-latest."
