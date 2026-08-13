cmake_minimum_required(VERSION 3.10)
project(rk3588-canfd-and-tcp-ip-communication VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(APP_NAME "${PROJECT_NAME}")
set(APP_VERSION "${PROJECT_VERSION}")
set(APP_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
if(NOT APP_BUILD_TYPE)
    set(APP_BUILD_TYPE "Release")
endif()

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE APP_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT APP_GIT_COMMIT)
    set(APP_GIT_COMMIT "")
endif()

execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE APP_GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT APP_GIT_BRANCH)
    set(APP_GIT_BRANCH "")
endif()

execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE APP_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(APP_GIT_STATUS)
    set(APP_GIT_DIRTY 1)
else()
    set(APP_GIT_DIRTY 0)
endif()

string(TIMESTAMP APP_BUILD_DATE "%Y-%m-%d %H:%M:%S" UTC)

file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/include/core")
file(WRITE "${PROJECT_SOURCE_DIR}/include/core/version.h"
    "#pragma once\n\n"
    "#define APP_NAME \"${APP_NAME}\"\n"
    "#define APP_VERSION \"${APP_VERSION}\"\n"
    "#define APP_GIT_COMMIT \"${APP_GIT_COMMIT}\"\n"
    "#define APP_GIT_BRANCH \"${APP_GIT_BRANCH}\"\n"
    "#define APP_GIT_DIRTY ${APP_GIT_DIRTY}\n"
    "#define APP_BUILD_TYPE \"${APP_BUILD_TYPE}\"\n"
    "#define APP_BUILD_DATE \"${APP_BUILD_DATE}\"\n"
)

set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "Release flags")
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g"      CACHE STRING "Debug flags")

if(CMAKE_BUILD_TYPE)
    add_compile_definitions(APP_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
else()
    add_compile_definitions(APP_BUILD_TYPE="Release")
endif()

option(DEBUG "Enable debug output to terminal" OFF)
if(DEBUG)
    add_compile_definitions(DEBUG)
endif()

set(SOURCES
    source/core/main.c
    source/core/log.c
    source/core/config.c
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

add_executable(rk3588-canfd-and-tcp-ip-communication ${SOURCES})

target_include_directories(rk3588-canfd-and-tcp-ip-communication PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)

# libnl3 headers are under /usr/include/libnl3
target_include_directories(rk3588-canfd-and-tcp-ip-communication PRIVATE /usr/include/libnl3)

set_target_properties(rk3588-canfd-and-tcp-ip-communication PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin"
)

target_link_libraries(rk3588-canfd-and-tcp-ip-communication PRIVATE pthread crypt)

find_library(LIBSYSTEMD systemd REQUIRED)
target_link_libraries(rk3588-canfd-and-tcp-ip-communication PRIVATE ${LIBSYSTEMD})

find_library(LIBNL3 nl-3 REQUIRED)
target_link_libraries(rk3588-canfd-and-tcp-ip-communication PRIVATE ${LIBNL3})

find_library(LIBNLROUTE3 nl-route-3 REQUIRED)
target_link_libraries(rk3588-canfd-and-tcp-ip-communication PRIVATE ${LIBNLROUTE3})
