# ---- IPC Library ----
add_library(installer-ipc STATIC
    ipc/json_rpc_codec.cpp
    ipc/ipc_server.cpp
    ipc/ipc_client.cpp
)

target_include_directories(installer-ipc PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(installer-ipc PUBLIC
    ${JSON_LIBRARIES}
    Threads::Threads
)

set_compiler_warnings(installer-ipc)

# ---- Core Service Executable ----
if(BUILD_CORE_SERVICE)
    add_executable(installer-core
        app/core_service_main.cpp
        app/service_setup.cpp
    )

    target_include_directories(installer-core PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(installer-core PRIVATE
        installer-ipc
        ${JSON_LIBRARIES}
        ${YAMLCPP_LIBRARIES}
        ${SPDLOG_LIBRARIES}
        Threads::Threads
    )

    if(yaml-cpp_FOUND)
        target_link_libraries(installer-core PRIVATE yaml-cpp)
    endif()

    set_compiler_warnings(installer-core)

    install(TARGETS installer-core RUNTIME DESTINATION sbin)
endif()
