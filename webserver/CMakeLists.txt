# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Standalone build:  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# Produces a ready-to-ship dashboard in build/dist/:  the hardware_monitor_cpp shared library +
# hardware_monitor_server.py + web/.  Run it with:  python hardware_monitor_server.py
cmake_minimum_required(VERSION 3.15)
project(hardware_monitor_cpp_web LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Core library (repo root), built with PIC so it links into this shared module.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_BINARY_DIR}/_hardware_monitor_cpp_core")

set(DIST "${CMAKE_BINARY_DIR}/dist")

add_library(hardware_monitor_cpp_c SHARED binding.cpp)
target_link_libraries(hardware_monitor_cpp_c PRIVATE hardware_monitor_cpp)
set_target_properties(hardware_monitor_cpp_c PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${DIST}"   # .dll (Windows)
    LIBRARY_OUTPUT_DIRECTORY "${DIST}")  # .so / .dylib (Linux/macOS)

# Assemble the ready-to-ship folder: server script + web assets next to the library.
add_custom_command(TARGET hardware_monitor_cpp_c POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/hardware_monitor_server.py" "${DIST}/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/web" "${DIST}/web"
    COMMENT "Assembling ready-to-ship dashboard in ${DIST}")
