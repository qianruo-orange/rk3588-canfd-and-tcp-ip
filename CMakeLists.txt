cmake_minimum_required(VERSION 3.10)
project(data_transport_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "Release flags")
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g"      CACHE STRING "Debug flags")

option(DEBUG "Enable debug output to terminal" OFF)
if(DEBUG)
    add_compile_definitions(DEBUG)
endif()

set(SOURCES
    source/core/main.c
    source/core/log.c
    source/core/config.c
    source/core/receive_data.c
    source/core/send_data.c
    source/can/can_socket.c
    source/net/tcp_server.c
    source/http/http.c
    source/http/http_logs.c
    source/http/http_api_system.c
    source/http/http_api_can.c
    source/http/http_api_config.c
    source/http/http_api_network.c
    source/http/http_api_video.c
    source/http/http_reboot.c
    source/video/video_stream.c
    source/watchdog/watchdog.c
)

add_executable(data_transport_test ${SOURCES})

target_include_directories(data_transport_test PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)

# libnl3 headers are under /usr/include/libnl3
target_include_directories(data_transport_test PRIVATE /usr/include/libnl3)

set_target_properties(data_transport_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin"
)

target_link_libraries(data_transport_test PRIVATE pthread crypt)

find_library(LIBSYSTEMD systemd REQUIRED)
target_link_libraries(data_transport_test PRIVATE ${LIBSYSTEMD})

find_library(LIBNL3 nl-3 REQUIRED)
target_link_libraries(data_transport_test PRIVATE ${LIBNL3})

find_library(LIBNLROUTE3 nl-route-3 REQUIRED)
target_link_libraries(data_transport_test PRIVATE ${LIBNLROUTE3})
