cmake_minimum_required(VERSION 3.20)
project(PortableManagerLite VERSION 0.2.2 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "PortableManager Lite requiere compilacion para arquitectura x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        -Wall
        -Wextra
        -Werror
        -municode
        -DUNICODE
        -D_UNICODE
        -DWIN32_LEAN_AND_MEAN
        -DNOMINMAX
    )
    add_link_options(
        -municode
        -static
        "-Wl,-Xlink=-swaprun:cd"
    )
elseif(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(
        /W4
        /WX
        /utf-8
        /permissive-
        /EHsc
        /DUNICODE
        /D_UNICODE
        /DWIN32_LEAN_AND_MEAN
        /DNOMINMAX
    )
    add_link_options(/SWAPRUN:CD)
endif()

set(PM_CORE_SOURCES
    src/core/path_resolver.cpp
    src/core/atomic_file.cpp
    src/core/config_manager.cpp
)

set(PM_RUNTIME_SOURCES
    src/runtime/manifest_manager.cpp
    src/runtime/session_manager.cpp
)

set(PM_GUARD_SOURCES
    src/jobs/job_manager.cpp
    src/guard/guard_process.cpp
)

add_executable(pm
    ${PM_GUARD_SOURCES}
    src/core/main.cpp
    src/guard/guard_entry.cpp
    ${PM_CORE_SOURCES}
    ${PM_RUNTIME_SOURCES}
)

target_include_directories(pm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pm PRIVATE
    kernel32
    user32
    advapi32
    shell32
    shlwapi
    version
    ole32
)

enable_testing()

add_executable(test_phase0_smoke
    tests/test_phase0_smoke.cpp
)
target_include_directories(test_phase0_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_phase0_smoke PRIVATE
    kernel32
    advapi32
)
add_test(NAME Phase0_SmokeTest COMMAND test_phase0_smoke $<TARGET_FILE:pm>)

add_executable(test_phase1_core
    tests/test_phase1_core.cpp
    ${PM_CORE_SOURCES}
)
target_include_directories(test_phase1_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_phase1_core PRIVATE
    kernel32
    advapi32
)
add_test(NAME Phase1_CoreTest COMMAND test_phase1_core $<TARGET_FILE:pm>)

add_executable(test_phase2_runtime
    tests/test_phase2_runtime.cpp
    ${PM_CORE_SOURCES}
    ${PM_RUNTIME_SOURCES}
)
target_include_directories(test_phase2_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_phase2_runtime PRIVATE
    kernel32
    advapi32
    ole32
)
add_test(NAME Phase2_RuntimeTest COMMAND test_phase2_runtime $<TARGET_FILE:pm>)

add_executable(test_phase3_guard
    tests/test_phase3_guard.cpp
    ${PM_CORE_SOURCES}
    ${PM_RUNTIME_SOURCES}
    ${PM_GUARD_SOURCES}
)
target_include_directories(test_phase3_guard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_options(test_phase3_guard PRIVATE "-Wl,-Xlink=-swaprun:cd")
target_link_libraries(test_phase3_guard PRIVATE kernel32 user32 advapi32 ole32)
add_test(NAME Phase3_GuardTest COMMAND test_phase3_guard $<TARGET_FILE:pm> "${CMAKE_CURRENT_BINARY_DIR}/phase3_windows_runtime.json")
set_tests_properties(Phase3_GuardTest PROPERTIES TIMEOUT 120)
