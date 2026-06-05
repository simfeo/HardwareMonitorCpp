# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Standalone build:  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# Produces a ready-to-ship console monitor in build/ (bin/).
cmake_minimum_required(VERSION 3.15)
project(idimus_monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# Static C/C++ runtime on MSVC so the exe ships without a redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Build the core library (repo root). Reused, not copied.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_BINARY_DIR}/_idimus_core")

add_executable(idimus_monitor main.cpp)
target_link_libraries(idimus_monitor PRIVATE idimus_hw)
set_target_properties(idimus_monitor PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
