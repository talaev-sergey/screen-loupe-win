cmake_minimum_required(VERSION 3.20)
project(ScreenLoupe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Статический CRT: на чистой Windows 11 нет VC++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(ScreenLoupe WIN32 src/main.cpp)
target_compile_definitions(ScreenLoupe PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_link_libraries(ScreenLoupe PRIVATE magnification)
target_compile_options(ScreenLoupe PRIVATE /utf-8 /W4)
