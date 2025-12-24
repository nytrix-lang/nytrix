cmake_minimum_required(VERSION 3.16)
project(nytrix VERSION 0.0.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(ny src/cmd/ny/main.c)
target_compile_definitions(ny PRIVATE NYTRIX_VERSION="0.0.1")
