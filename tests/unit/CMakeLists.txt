# Unit Tests for ReXGlue core components

add_executable(unit_tests
    memory/heap_allocation_test.cpp
    memory/access_violation_recovery_test.cpp
    system/mmio_handler_test.cpp
    system/missing_function_test.cpp
    system/cache_drive_test.cpp
    kernel/object_table_test.cpp
    kernel/xboxkrnl_io_info_test.cpp
    core/cvar_test.cpp
    core/fiber_test.cpp
    core/sha256_test.cpp
    core/stream_test.cpp
    core/byte_order_test.cpp
    core/chrono_test.cpp
    core/content_probe_test.cpp
    core/threading_test.cpp
    codegen/config_test.cpp
    codegen/template_registry_test.cpp
    codegen/jump_table_bias_test.cpp
)

target_include_directories(unit_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

# rexsystem references rexkernel symbols; rescan the pair so the static link
# resolves them regardless of the generated link order. The `$<LINK_GROUP>`
# generator expression is not supported by every toolchain (e.g. MSVC), so it
# is only added on non-Windows hosts (cycle-9 C3); other presets are unaffected.
set(unit_tests_link_libs
    rexcore
    rexcodegen
    rexkernel
)
if(NOT WIN32)
    list(APPEND unit_tests_link_libs "$<LINK_GROUP:RESCAN,rexkernel,rexsystem>")
endif()

target_link_libraries(unit_tests PRIVATE
    ${unit_tests_link_libs}
    Catch2::Catch2WithMain
)

set_target_properties(unit_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/bin
)

# Discover tests
include(Catch)
catch_discover_tests(unit_tests
    TEST_PREFIX "unit."
    PROPERTIES
        LABELS "unit"
        TIMEOUT 60
)
