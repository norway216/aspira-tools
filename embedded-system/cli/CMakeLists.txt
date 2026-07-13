# ---- CLI Executable ----
add_executable(installer-cli
    cli_main.cpp
    cli_commands.cpp
)

target_include_directories(installer-cli PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(installer-cli PRIVATE
    installer-ipc
    ${JSON_LIBRARIES}
    ${SPDLOG_LIBRARIES}
    Threads::Threads
)

set_compiler_warnings(installer-cli)

install(TARGETS installer-cli RUNTIME DESTINATION bin)
