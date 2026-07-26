cmake_minimum_required(VERSION 3.16)
project(HeapForge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# header-only 核心库
add_library(heapforge INTERFACE)
add_library(heapforge::heapforge ALIAS heapforge)
target_include_directories(heapforge INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(heapforge INTERFACE cxx_std_17)

# 作为子项目被 add_subdirectory 引入时，不构建 demo/测试
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(HEAPFORGE_IS_TOP_LEVEL ON)
else()
  set(HEAPFORGE_IS_TOP_LEVEL OFF)
endif()
option(HEAPFORGE_BUILD_TESTS "Build HeapForge tests and demo" ${HEAPFORGE_IS_TOP_LEVEL})

if(HEAPFORGE_BUILD_TESTS)
  # Debug 构建保留帧指针，保证 backtrace 完整
  if(NOT MSVC)
    add_compile_options(-Wall -Wextra -fno-omit-frame-pointer)
  endif()

  # 演示程序
  add_executable(heapforge_demo examples/demo.cpp)
  target_link_libraries(heapforge_demo PRIVATE heapforge)

  # 测试
  enable_testing()
  add_executable(heapforge_tests tests/test_heapforge.cpp)
  target_link_libraries(heapforge_tests PRIVATE heapforge)
  add_test(NAME heapforge_tests COMMAND heapforge_tests)
endif()

# 安装 & 导出：支持 find_package(heapforge)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS heapforge EXPORT heapforgeTargets)
install(EXPORT heapforgeTargets
  NAMESPACE heapforge::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/heapforge)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/heapforgeConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/heapforgeConfig.cmake
  "include(\${CMAKE_CURRENT_LIST_DIR}/heapforgeTargets.cmake)\n")
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/heapforgeConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/heapforgeConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/heapforge)
