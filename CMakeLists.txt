# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
cmake_minimum_required(VERSION 3.15)
project(idimus_hw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB_RECURSE IDIMUS_HW_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(idimus_hw STATIC ${IDIMUS_HW_SOURCES})
target_include_directories(idimus_hw
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(MSVC)
    target_compile_options(idimus_hw PRIVATE /utf-8 /W3)
    target_compile_definitions(idimus_hw PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
    target_compile_options(idimus_hw PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    target_link_libraries(idimus_hw PUBLIC advapi32 iphlpapi setupapi powrprof ntdll)
endif()

if(APPLE)
    find_library(IOKIT_LIBRARY IOKit REQUIRED)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
    target_link_libraries(idimus_hw PUBLIC ${IOKIT_LIBRARY} ${COREFOUNDATION_LIBRARY})
    # IOReport reader uses a clang block callback.
    target_compile_options(idimus_hw PRIVATE -fblocks)
endif()

add_executable(idimus_hw_dump "${CMAKE_CURRENT_SOURCE_DIR}/examples/idimus_hw_dump.cpp")
target_link_libraries(idimus_hw_dump PRIVATE idimus_hw)
