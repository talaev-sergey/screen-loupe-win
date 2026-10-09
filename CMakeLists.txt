cmake_minimum_required(VERSION 3.20)
project(ScreenLoupe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Статический CRT: на чистой Windows 11 нет VC++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Версию вшивает CI из тега; по ней exe решает, новее ли последний релиз.
set(APP_VERSION "0.0.0" CACHE STRING "Version embedded into the exe")

add_executable(ScreenLoupe WIN32 src/main.cpp)
target_compile_definitions(ScreenLoupe PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00
                           APP_VERSION="${APP_VERSION}")
target_link_libraries(ScreenLoupe PRIVATE magnification winhttp bcrypt)
target_compile_options(ScreenLoupe PRIVATE /utf-8 /W4)
