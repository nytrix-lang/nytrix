# etc/tools/toolchain-arm.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_PREFIX arm-linux-gnueabihf)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)

set(CMAKE_FIND_ROOT_PATH "/usr/${TOOLCHAIN_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -I/usr/${TOOLCHAIN_PREFIX}/include" CACHE STRING "C compiler flags" FORCE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -I/usr/${TOOLCHAIN_PREFIX}/include" CACHE STRING "CXX compiler flags" FORCE)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)

set(CMAKE_CROSSCOMPILING_EXPLICITLY TRUE)

set(NYTRIX_LLVM_INCLUDE "/usr/${TOOLCHAIN_PREFIX}/include" CACHE STRING "LLVM include root (directory that contains llvm-c/)" FORCE)
set(NYTRIX_LLVM_CFLAGS "-I/usr/${TOOLCHAIN_PREFIX}/include" CACHE STRING "LLVM C flags list" FORCE)

file(GLOB LLVM_STATIC_LIBS "/usr/${TOOLCHAIN_PREFIX}/lib/libLLVM*.a")

set(LLVM_LIBS "")
foreach(lib_path ${LLVM_STATIC_LIBS})
    get_filename_component(lib_name ${lib_path} NAME_WE)
    string(REPLACE "lib" "" lib_name_no_prefix ${lib_name})
    list(APPEND LLVM_LIBS "${lib_name_no_prefix}")
endforeach()
list(APPEND LLVM_LIBS "stdc++")
list(APPEND LLVM_LIBS "gcc_s")
list(APPEND LLVM_LIBS "m")      # Math library (often needed)
list(APPEND LLVM_LIBS "pthread")# Pthread library (often needed)

set(LLVM_LIBS "${LLVM_LIBS}" CACHE INTERNAL "List of LLVM libraries for linking" FORCE)
