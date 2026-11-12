# Toolchain file para CMake con LLVM-MinGW portable (Windows x86_64 UCRT)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(PM_TOOLCHAIN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../tools/toolchains/llvm-mingw" ABSOLUTE)

set(CMAKE_C_COMPILER   "${PM_TOOLCHAIN_ROOT}/bin/clang.exe")
set(CMAKE_CXX_COMPILER "${PM_TOOLCHAIN_ROOT}/bin/clang++.exe")
set(CMAKE_RC_COMPILER  "${PM_TOOLCHAIN_ROOT}/bin/x86_64-w64-mingw32-windres.exe")
set(CMAKE_AR           "${PM_TOOLCHAIN_ROOT}/bin/llvm-ar.exe")
set(CMAKE_RANLIB       "${PM_TOOLCHAIN_ROOT}/bin/llvm-ranlib.exe")
set(CMAKE_STRIP        "${PM_TOOLCHAIN_ROOT}/bin/llvm-strip.exe")

set(CMAKE_FIND_ROOT_PATH "${PM_TOOLCHAIN_ROOT}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
