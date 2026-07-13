# ============================================================================
# tests/CMakeLists.txt
# Test build configuration for the embedded Linux installer.
# ============================================================================

# ---- Gather test sources ----

file(GLOB UNIT_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp
)

file(GLOB INTEGRATION_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cpp
)

file(GLOB TEST_HELPER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/helpers/*.cpp
)

# ---- GTest setup ----

if(GTest_FOUND)
    message(STATUS "Using system GTest")
    set(TEST_LINK_LIBRARIES GTest::GTest GTest::Main)
else()
    message(STATUS "Using bundled minimal test framework (GTest not found)")
    # Bundled minimal framework provides main() and basic assertions
    add_library(test_framework STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/helpers/test_framework.cpp
    )
    target_include_directories(test_framework PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/helpers
    )
    set(TEST_LINK_LIBRARIES test_framework)
endif()

# ---- Unit tests ----

if(UNIT_TEST_SOURCES)
    add_executable(test_unit
        ${UNIT_TEST_SOURCES}
        ${TEST_HELPER_SOURCES}
    )

    target_include_directories(test_unit PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/helpers
    )

    target_link_libraries(test_unit PRIVATE
        installer_core_lib
        installer_common
        Threads::Threads
        ${TEST_LINK_LIBRARIES}
    )

    target_compile_definitions(test_unit PRIVATE
        UNIT_TEST
    )

    add_test(NAME UnitTests COMMAND test_unit)
endif()

# ---- Integration tests ----

if(INTEGRATION_TEST_SOURCES)
    add_executable(test_integration
        ${INTEGRATION_TEST_SOURCES}
        ${TEST_HELPER_SOURCES}
    )

    target_include_directories(test_integration PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/helpers
    )

    target_link_libraries(test_integration PRIVATE
        installer_core_lib
        installer_common
        Threads::Threads
        ${TEST_LINK_LIBRARIES}
    )

    target_compile_definitions(test_integration PRIVATE
        INTEGRATION_TEST
    )

    add_test(NAME IntegrationTests COMMAND test_integration)
endif()

# ---- Fault injection tests ----

file(GLOB FAULT_INJECT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/fault_injection/*.cpp
)

if(FAULT_INJECT_SOURCES AND ENABLE_FAULT_INJECT)
    add_executable(test_fault_injection
        ${FAULT_INJECT_SOURCES}
        ${TEST_HELPER_SOURCES}
    )

    target_include_directories(test_fault_injection PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/helpers
    )

    target_link_libraries(test_fault_injection PRIVATE
        installer_core_lib
        installer_common
        Threads::Threads
        ${TEST_LINK_LIBRARIES}
    )

    target_compile_definitions(test_fault_injection PRIVATE
        FAULT_INJECT_TEST
        ENABLE_FAULT_INJECT
    )

    add_test(NAME FaultInjectionTests COMMAND test_fault_injection)
endif()

# ---- Convenience targets ----

add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_unit test_integration
    COMMENT "Running all installer tests"
)
