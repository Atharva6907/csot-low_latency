cmake_minimum_required(VERSION 3.20)
project(csot_quant_platform CXX)

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference)
add_compile_options($<$<CONFIG:Release>:-O2>)
add_compile_options($<$<CONFIG:Release>:-march=native>)
add_compile_options($<$<CONFIG:Release>:-fno-omit-frame-pointer>)

option(ENABLE_SANITIZERS "Build with AddressSanitizer + UBSan" OFF)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

option(ENABLE_LTO "Enable link-time optimization for Release" ON)
if(ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_ok OUTPUT lto_err)
    if(lto_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "LTO not supported: ${lto_err}")
    endif()
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

add_library(spec_strategy_obj OBJECT strategies/spec_strategy.cpp)

add_executable(quant_runner src/engine.cpp)
target_link_libraries(quant_runner PRIVATE spec_strategy_obj)

add_library(spec_strategy SHARED strategies/spec_strategy.cpp)
set_target_properties(spec_strategy PROPERTIES PREFIX "")

add_executable(quant_bench bench/bench.cpp)
target_link_libraries(quant_bench PRIVATE spec_strategy_obj)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE} | Sanitizers: ${ENABLE_SANITIZERS} | LTO: ${ENABLE_LTO} | Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
