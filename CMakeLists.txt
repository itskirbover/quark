cmake_minimum_required(VERSION 3.16)
project(quark VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

# Embedded main-menu logo (pure CMake, no extra tools): converts
# src/ui/Q.png into a byte array header at configure time.
file(READ "${CMAKE_SOURCE_DIR}/src/ui/Q.png" QUARK_LOGO_HEX HEX)
string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," QUARK_LOGO_BYTES
       "${QUARK_LOGO_HEX}")
file(WRITE "${CMAKE_BINARY_DIR}/gen/QuarkLogo.hpp"
    "#pragma once\n"
    "// Generated from src/ui/Q.png — do not edit.\n"
    "#include <cstddef>\n"
    "namespace quark_logo {\n"
    "inline constexpr unsigned char bytes[] = {\n    ${QUARK_LOGO_BYTES}\n};\n"
    "inline constexpr std::size_t size = sizeof(bytes);\n"
    "}  // namespace quark_logo\n")

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

target_include_directories(quark PRIVATE src "${CMAKE_BINARY_DIR}/gen")
