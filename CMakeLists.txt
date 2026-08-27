cmake_minimum_required(VERSION 3.10)
project(rk3588-canfd-and-tcp-ip VERSION 1.1.0 LANGUAGES C)

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

# 通配符收集源文件与头文件（新增模块无需再改 CMakeLists）
file(GLOB APP_SOURCES CONFIGURE_DEPENDS source/**/*.c)
file(GLOB APP_HEADERS CONFIGURE_DEPENDS include/**/*.h)

add_executable(rk3588-canfd-and-tcp-ip ${APP_SOURCES} ${APP_HEADERS})

target_include_directories(rk3588-canfd-and-tcp-ip PRIVATE ${PROJECT_SOURCE_DIR}/source ${PROJECT_SOURCE_DIR}/include)

# libnl3 headers are under /usr/include/libnl3
target_include_directories(rk3588-canfd-and-tcp-ip PRIVATE /usr/include/libnl3)

set_target_properties(rk3588-canfd-and-tcp-ip PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin"
)

target_link_libraries(rk3588-canfd-and-tcp-ip PRIVATE pthread crypt m)

find_library(LIBSYSTEMD systemd REQUIRED)
target_link_libraries(rk3588-canfd-and-tcp-ip PRIVATE ${LIBSYSTEMD})

find_library(LIBNL3 nl-3 REQUIRED)
target_link_libraries(rk3588-canfd-and-tcp-ip PRIVATE ${LIBNL3})

find_library(LIBNLROUTE3 nl-route-3 REQUIRED)
target_link_libraries(rk3588-canfd-and-tcp-ip PRIVATE ${LIBNLROUTE3})

# RKNN NPU 运行时（librknnrt.so，板载 /usr/lib）与 libjpeg-turbo（帧编解码）
find_library(LIBRKNNRT rknnrt REQUIRED)
target_link_libraries(rk3588-canfd-and-tcp-ip PRIVATE ${LIBRKNNRT})

find_library(LIBJPEG jpeg REQUIRED)
target_link_libraries(rk3588-canfd-and-tcp-ip PRIVATE ${LIBJPEG})

# FFmpeg（可选）：提供 h264_rkmpp 硬件编码后端（Rockchip MPP）。
# 系统装有带 rkmpp 的 libavcodec 时自动启用；否则编译 V4L2 后端。
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(AVCODEC QUIET IMPORTED_TARGET libavcodec libavutil)
endif()
if(AVCODEC_FOUND)
    add_compile_definitions(HAVE_AVCODEC)
    target_link_libraries(rk3588-canfd-and-tcp-ip PRIVATE PkgConfig::AVCODEC)
    message(STATUS "FFmpeg found: h264_rkmpp backend enabled")
endif()
