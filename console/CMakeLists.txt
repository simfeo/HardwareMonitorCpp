# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Standalone build:  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# Produces a ready-to-ship console monitor in build/ (bin/).
cmake_minimum_required(VERSION 3.15)
project(hardware_monitor_console LANGUAGES CXX)

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
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_BINARY_DIR}/_hardware_monitor_cpp_core")

add_executable(hardware_monitor_console main.cpp)
target_link_libraries(hardware_monitor_console PRIVATE hardware_monitor_cpp)
set_target_properties(hardware_monitor_console PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
