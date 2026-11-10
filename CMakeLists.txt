cmake_minimum_required(VERSION 3.15)
project(SignalForge VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

enable_testing()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_subdirectory(external/Catch2)

# =========================================================
# Subdirectories
# =========================================================
if(NOT DEFINED PLATFORM)
    message(STATUS "PLATFORM not set, nothing to build")
elseif(PLATFORM STREQUAL "host")
    message(STATUS "PLATFORM set to 'host': building host files")

    # =====================================================
    # System Dependencies
    # =====================================================
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

    # =====================================================
    # Host Directories
    # =====================================================
    add_subdirectory(shared)
    add_subdirectory(host)
elseif(PLATFORM STREQUAL "kria")
    message(STATUS "PLATFORM set to 'kria': building kria files")

    # =====================================================
    # Device Directories
    # =====================================================
    add_subdirectory(shared)
    add_subdirectory(kria)
else()
    message(STATUS "Unknown PLATFORM: ${PLATFORM}")
endif()
