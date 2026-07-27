cmake_minimum_required(VERSION 3.24)
project(bitmap_exchange_showcase VERSION 1.1.0 LANGUAGES CXX)

option(BX_NATIVE "Optimize for the build machine" ON)
option(BX_IPO "Enable interprocedural optimization when supported" ON)
option(BX_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(BX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# Off by default on purpose. The engine is a single dependency-free header, and
# a header builds on every compiler that can build the tests. Named modules do
# not: GCC's implementation is still incomplete enough that a modules-only
# library would simply fail to build for a large share of anyone who cloned
# this. So the module is an alternative front door rather than the only one --
# `-DBX_MODULE=ON` compiles and tests through `bitmap_exchange.cppm` instead.
option(BX_MODULE "Also build and test the C++ module interface" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(bitmap_exchange INTERFACE)
target_include_directories(bitmap_exchange INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")

if(MSVC)
    # C4324: "structure was padded due to alignment specifier". The padding is
    # the intent of the alignas(64) on SideState, so the warning is noise here.
    target_compile_options(bitmap_exchange INTERFACE /W4 /permissive- /EHsc /wd4324)
    if(BX_WARNINGS_AS_ERRORS)
        target_compile_options(bitmap_exchange INTERFACE /WX)
    endif()
    if(BX_NATIVE)
        target_compile_options(bitmap_exchange INTERFACE /arch:AVX2)
    endif()
else()
    target_compile_options(bitmap_exchange INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-DNDEBUG>)
    if(BX_WARNINGS_AS_ERRORS)
        target_compile_options(bitmap_exchange INTERFACE -Werror)
    endif()
    if(BX_NATIVE)
        target_compile_options(bitmap_exchange INTERFACE $<$<CONFIG:Release>:-march=native>)
    endif()
    if(BX_SANITIZERS)
        target_compile_options(bitmap_exchange INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer)
        target_link_options(bitmap_exchange INTERFACE -fsanitize=address,undefined)
    endif()
endif()

function(bx_add_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE bitmap_exchange)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

enable_testing()
bx_add_test(bx_test_bitmap_l2 tests/test_bitmap_l2.cpp)
bx_add_test(bx_test_l3 tests/test_l3.cpp)
bx_add_test(bx_test_differential tests/test_differential.cpp)

add_executable(bx_bench bench.cpp)
target_link_libraries(bx_bench PRIVATE bitmap_exchange)
if(UNIX AND NOT APPLE)
    target_link_libraries(bx_bench PRIVATE pthread)
endif()

if(BX_MODULE)
    if(CMAKE_VERSION VERSION_LESS 3.28)
        message(FATAL_ERROR "BX_MODULE needs CMake 3.28 or newer for C++ module support")
    endif()
    add_library(bitmap_exchange_module STATIC)
    target_sources(bitmap_exchange_module
        PUBLIC FILE_SET CXX_MODULES FILES bitmap_exchange.cppm)
    target_link_libraries(bitmap_exchange_module PUBLIC bitmap_exchange)

    # The same assertions as tests/test_l3.cpp, reached through `import` rather
    # than `#include`. It is a build-level check: if the module exports the wrong
    # set of names, or exports them without their definitions, this fails to
    # compile, which is the failure mode worth catching.
    add_executable(bx_test_module tests/test_module.cpp)
    target_link_libraries(bx_test_module PRIVATE bitmap_exchange_module)
    add_test(NAME bx_test_module COMMAND bx_test_module)
endif()

if(BX_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT BX_IPO_SUPPORTED OUTPUT BX_IPO_ERROR LANGUAGES CXX)
    if(BX_IPO_SUPPORTED)
        set_property(
            TARGET bx_test_bitmap_l2 bx_test_l3 bx_test_differential bx_bench
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(WARNING "IPO disabled: ${BX_IPO_ERROR}")
    endif()
endif()
