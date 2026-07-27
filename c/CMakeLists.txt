cmake_minimum_required(VERSION 3.16)
project(heapforge_c LANGUAGES C VERSION 1.0.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(HEAPFORGE_C_BUILD_TESTS "Build HeapForge (C) tests and demo" ON)

# ---- 静态库 / static library ----
file(GLOB HEAPFORGE_C_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
add_library(heapforge_c STATIC ${HEAPFORGE_C_SOURCES})
add_library(heapforge::c ALIAS heapforge_c)

target_include_directories(heapforge_c PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

find_package(Threads REQUIRED)
target_link_libraries(heapforge_c PUBLIC Threads::Threads)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(heapforge_c PRIVATE -Wall -Wextra -fno-omit-frame-pointer)
endif()

# ---- 测试与演示 / tests and demo ----
if(HEAPFORGE_C_BUILD_TESTS)
    enable_testing()

    add_executable(heapforge_c_tests tests/test_heapforge_c.c)
    target_link_libraries(heapforge_c_tests PRIVATE heapforge_c)
    add_test(NAME heapforge_c_tests COMMAND heapforge_c_tests)

    add_executable(heapforge_c_demo examples/demo.c)
    target_link_libraries(heapforge_c_demo PRIVATE heapforge_c)
endif()

# ---- 安装 / install ----
include(GNUInstallDirs)
install(TARGETS heapforge_c EXPORT heapforge_c-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT heapforge_c-targets
        FILE heapforge_c-config.cmake
        NAMESPACE heapforge::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/heapforge_c)
