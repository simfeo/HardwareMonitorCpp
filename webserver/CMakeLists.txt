# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Standalone build:  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# Produces a ready-to-ship dashboard in build/dist/:  the idimus_hw shared library +
# idimus_server.py + web/.  Run it with:  python idimus_server.py
cmake_minimum_required(VERSION 3.15)
project(idimus_hw_web LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Core library (repo root), built with PIC so it links into this shared module.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_BINARY_DIR}/_idimus_core")

set(DIST "${CMAKE_BINARY_DIR}/dist")

add_library(idimus_hw_c SHARED binding.cpp)
target_link_libraries(idimus_hw_c PRIVATE idimus_hw)
set_target_properties(idimus_hw_c PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${DIST}"   # .dll (Windows)
    LIBRARY_OUTPUT_DIRECTORY "${DIST}")  # .so / .dylib (Linux/macOS)

# Assemble the ready-to-ship folder: server script + web assets next to the library.
add_custom_command(TARGET idimus_hw_c POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/idimus_server.py" "${DIST}/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/web" "${DIST}/web"
    COMMENT "Assembling ready-to-ship dashboard in ${DIST}")
