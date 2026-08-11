# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
cmake_minimum_required(VERSION 3.15)
project(hardware_monitor_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB_RECURSE HARDWARE_MONITOR_CPP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(hardware_monitor_cpp STATIC ${HARDWARE_MONITOR_CPP_SOURCES})
target_include_directories(hardware_monitor_cpp
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Allow linking the static core into a shared library (the Python C-ABI module).
set_target_properties(hardware_monitor_cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(hardware_monitor_cpp PRIVATE /utf-8 /W3)
    target_compile_definitions(hardware_monitor_cpp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
    target_compile_options(hardware_monitor_cpp PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    target_link_libraries(hardware_monitor_cpp PUBLIC advapi32 iphlpapi setupapi powrprof ntdll dxgi pdh)
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(hardware_monitor_cpp PUBLIC Threads::Threads ${CMAKE_DL_LIBS}) # dl for NVML dlopen
endif()

if(APPLE)
    find_library(IOKIT_LIBRARY IOKit REQUIRED)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
    target_link_libraries(hardware_monitor_cpp PUBLIC ${IOKIT_LIBRARY} ${COREFOUNDATION_LIBRARY})
    # IOReport reader uses a clang block callback.
    target_compile_options(hardware_monitor_cpp PRIVATE -fblocks)
endif()

add_executable(hardware_monitor_cpp_dump "${CMAKE_CURRENT_SOURCE_DIR}/examples/hardware_monitor_cpp_dump.cpp")
target_link_libraries(hardware_monitor_cpp_dump PRIVATE hardware_monitor_cpp)
