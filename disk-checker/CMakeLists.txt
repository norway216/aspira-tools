cmake_minimum_required(VERSION 3.10)
project(disk_health
    VERSION 1.0.0
    DESCRIPTION "Medical-Grade Disk Health Monitoring Daemon"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG   "-g -O0 -DDEBUG")

set(SOURCES
    src/main.c
    src/config.c
    src/daemon.c
    src/scanner.c
    src/smart_ata.c
    src/smart_nvme.c
    src/io_metrics.c
    src/health_score.c
    src/logger.c
    src/json_builder.c
    src/fault_detect.c
    src/policy_engine.c
    src/failure_predict.c
    src/self_healing.c
)

add_executable(disk_health ${SOURCES})
target_include_directories(disk_health PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(disk_health m)

install(TARGETS disk_health
    RUNTIME DESTINATION /usr/local/bin
)

install(FILES systemd/disk_healthd.service
    DESTINATION /etc/systemd/system
)

# Tests
enable_testing()
add_test(NAME test_scanner      COMMAND ${CMAKE_SOURCE_DIR}/tests/test_scanner.sh)
add_test(NAME test_smart_ata    COMMAND ${CMAKE_SOURCE_DIR}/tests/test_smart_ata.sh)
add_test(NAME test_io_metrics   COMMAND ${CMAKE_SOURCE_DIR}/tests/test_io_metrics.sh)
add_test(NAME test_health_score COMMAND ${CMAKE_SOURCE_DIR}/tests/test_health_score.sh)
add_test(NAME test_json_output  COMMAND ${CMAKE_SOURCE_DIR}/tests/test_json_output.sh)
add_test(NAME test_daemon       COMMAND ${CMAKE_SOURCE_DIR}/tests/test_daemon.sh)
