cmake_minimum_required(VERSION 3.16)
project(quark VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(quark
  src/main.cpp
  src/Terminal.cpp
  src/Input.cpp
  src/Buffer.cpp
  src/Markdown.cpp
  src/Kitty.cpp
  src/Renderer.cpp
  src/Editor.cpp
  src/Menu.cpp
)

target_include_directories(quark PRIVATE src)
