cmake_minimum_required(VERSION 3.20)
project(PortableManagerLite VERSION 0.2.2 LANGUAGES CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "PortableManager Lite requiere compilacion para arquitectura x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-municode>")
    add_link_options(
        -municode
        -static
        "-Wl,-Xlink=-swaprun:cd"
    )
elseif(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/utf-8;/permissive-;/EHsc>")
    add_link_options(/SWAPRUN:CD)
endif()
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

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
    src/junctions/junction_manager.cpp
    src/registry/registry_manager.cpp
    src/guard/guard_process.cpp
    src/cleanup/cleanup_engine.cpp
)

add_executable(pml
    src/pmd/pmd_module.h
    src/guard/guard_supervisor.cpp
    src/applications/session_host.cpp
    src/applications/app_paths.cpp
    src/applications/applications_module.cpp
    src/applications/package_manager.cpp
    src/applications/app_command.cpp
    src/pmd/pmd_module.cpp
    ${PM_GUARD_SOURCES}
    src/learning/learning_engine.cpp
    src/core/main.cpp
    src/guard/guard_entry.cpp
    ${PM_CORE_SOURCES}
    ${PM_RUNTIME_SOURCES}
)

target_include_directories(pml PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pml PRIVATE
    kernel32
    user32
    gdi32
    comdlg32
    advapi32
    shell32
    shlwapi
    version
    ole32
    bcrypt
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
add_test(NAME Phase0_SmokeTest COMMAND test_phase0_smoke $<TARGET_FILE:pml>)

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
add_test(NAME Phase1_CoreTest COMMAND test_phase1_core $<TARGET_FILE:pml>)

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
add_test(NAME Phase2_RuntimeTest COMMAND test_phase2_runtime $<TARGET_FILE:pml>)

add_executable(test_phase3_guard
    tests/test_phase3_guard.cpp
    ${PM_CORE_SOURCES}
    ${PM_RUNTIME_SOURCES}
    ${PM_GUARD_SOURCES}
)
target_include_directories(test_phase3_guard PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_options(test_phase3_guard PRIVATE "-Wl,-Xlink=-swaprun:cd")
target_link_libraries(test_phase3_guard PRIVATE kernel32 user32 advapi32 ole32 bcrypt)
add_test(NAME Phase3_GuardTest COMMAND test_phase3_guard $<TARGET_FILE:pml> "${CMAKE_CURRENT_BINARY_DIR}/phase3_windows_runtime.json")
set_tests_properties(Phase3_GuardTest PROPERTIES TIMEOUT 120)

add_executable(test_phase4_junctions
    tests/test_phase4_junctions.cpp
    ${PM_CORE_SOURCES}
    ${PM_RUNTIME_SOURCES}
    ${PM_GUARD_SOURCES}
)
target_include_directories(test_phase4_junctions PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_phase4_junctions PRIVATE kernel32 user32 advapi32 ole32 bcrypt)
add_test(NAME Phase4_JunctionsTest COMMAND test_phase4_junctions $<TARGET_FILE:pml> "${CMAKE_CURRENT_BINARY_DIR}/phase4_windows_runtime.json")
set_tests_properties(Phase4_JunctionsTest PROPERTIES TIMEOUT 120)

add_executable(test_phase5_registry
    tests/test_phase5_registry.cpp
    ${PM_CORE_SOURCES}
    ${PM_RUNTIME_SOURCES}
    ${PM_GUARD_SOURCES}
)
target_include_directories(test_phase5_registry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_phase5_registry PRIVATE kernel32 user32 advapi32 ole32 bcrypt)
add_test(NAME Phase5_RegistryTest COMMAND test_phase5_registry $<TARGET_FILE:pml> "${CMAKE_CURRENT_BINARY_DIR}/phase5_windows_runtime.json")
set_tests_properties(Phase5_RegistryTest PROPERTIES TIMEOUT 120)

add_executable(test_phase6_learning
    src/guard/guard_supervisor.cpp
    tests/test_phase6_learning.cpp
    tests/phase6_fixture.rc
    src/learning/learning_engine.cpp
    src/guard/guard_entry.cpp
    ${PM_CORE_SOURCES}
    ${PM_RUNTIME_SOURCES}
    ${PM_GUARD_SOURCES}
)
target_include_directories(test_phase6_learning PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_phase6_learning PRIVATE kernel32 user32 advapi32 ole32 bcrypt version)
add_test(NAME Phase6_LearningTest COMMAND test_phase6_learning "${CMAKE_CURRENT_BINARY_DIR}/phase6_windows_runtime.json")
set_tests_properties(Phase6_LearningTest PROPERTIES TIMEOUT 120)

add_executable(test_phase7_cleanup
    tests/test_phase7_cleanup.cpp
    ${PM_CORE_SOURCES}
    ${PM_RUNTIME_SOURCES}
    ${PM_GUARD_SOURCES}
)
target_include_directories(test_phase7_cleanup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_phase7_cleanup PRIVATE kernel32 user32 advapi32 ole32 bcrypt)
add_test(NAME Phase7_CleanupTest COMMAND test_phase7_cleanup $<TARGET_FILE:pml> "${CMAKE_CURRENT_BINARY_DIR}/phase7_windows_runtime.json")
set_tests_properties(Phase7_CleanupTest PROPERTIES TIMEOUT 120)

add_executable(test_phase8_integration
    tests/test_phase8_integration.cpp
    tests/phase6_fixture.rc
    ${PM_CORE_SOURCES}
    ${PM_RUNTIME_SOURCES}
)
target_include_directories(test_phase8_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_phase8_integration PRIVATE kernel32 user32 advapi32 ole32)
add_test(NAME Phase8_IntegrationTest COMMAND test_phase8_integration $<TARGET_FILE:pml> "${CMAKE_CURRENT_BINARY_DIR}/phase8_windows_runtime.json")
set_tests_properties(Phase8_IntegrationTest PROPERTIES TIMEOUT 150)

if(NOT CMAKE_CROSSCOMPILING)
    foreach(suite IN ITEMS Phase3_GuardTest Phase4_JunctionsTest Phase5_RegistryTest Phase6_LearningTest Phase7_CleanupTest Phase8_IntegrationTest)
        set(suite_temp "${CMAKE_CURRENT_BINARY_DIR}/temp/${suite}")
        file(MAKE_DIRECTORY "${suite_temp}")
        file(TO_NATIVE_PATH "${suite_temp}" suite_temp_native)
        set_tests_properties(${suite} PROPERTIES ENVIRONMENT "TEMP=${suite_temp_native};TMP=${suite_temp_native}")
    endforeach()
endif()
