cmake_minimum_required(VERSION 3.16)
project(MiniGameServer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 引用 HeapForge：方式二 —— add_subdirectory 指向库根目录
# （HEAPFORGE_BUILD_TESTS 自动关闭，不会构建库自己的测试/demo）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. heapforge_build)

add_executable(game_server src/main.cpp)
target_link_libraries(game_server PRIVATE heapforge::heapforge)

if(NOT MSVC)
  # 保留帧指针，让 DebugHeap 的泄漏调用栈完整
  target_compile_options(game_server PRIVATE -fno-omit-frame-pointer)
endif()
