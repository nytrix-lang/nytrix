# etc/tools/toolchain-windows.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

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
set(CMAKE_CROSSCOMPILING_EMULATOR "wine")

set(ZLIB_INCLUDE_DIR "/usr/${TOOLCHAIN_PREFIX}/include" CACHE PATH "ZLIB include directory for MinGW" FORCE)
set(ZLIB_LIBRARY "/usr/${TOOLCHAIN_PREFIX}/lib/libz.a" CACHE FILEPATH "ZLIB library for MinGW" FORCE)
find_package(ZLIB REQUIRED)

set(THREADS_INCLUDE_DIR "/usr/${TOOLCHAIN_PREFIX}/include" CACHE PATH "Threads include directory for MinGW" FORCE)
set(THREADS_LIBRARIES "/usr/${TOOLCHAIN_PREFIX}/lib/libwinpthread.a;/usr/${TOOLCHAIN_PREFIX}/lib/libpthread.a" CACHE FILEPATH "Threads library for MinGW" FORCE)

set(NYTRIX_LLVM_INCLUDE "/usr/${TOOLCHAIN_PREFIX}/include" CACHE STRING "LLVM include root (directory that contains llvm-c/)" FORCE)
set(NYTRIX_LLVM_CFLAGS "-I/usr/${TOOLCHAIN_PREFIX}/include" CACHE STRING "LLVM C flags list" FORCE)

file(GLOB LLVM_STATIC_LIBS "/usr/${TOOLCHAIN_PREFIX}/lib/libLLVM*.a")

set(NYTRIX_LLLL_LIBS_NO_PREFIX "")
foreach(lib_path ${LLVM_STATIC_LIBS})
    get_filename_component(lib_name ${lib_path} NAME_WE)
    string(REPLACE "lib" "" lib_name_no_prefix ${lib_name})
    list(APPEND NYTRIX_LLLL_LIBS_NO_PREFIX ${lib_name_no_prefix})
endforeach()

set(NYTRIX_LLVM_LDFLAGS "" CACHE INTERNAL "LLVM linker flags list" FORCE)
foreach(lib ${NYTRIX_LLLL_LIBS_NO_PREFIX})
    list(APPEND NYTRIX_LLVM_LDFLAGS "-l${lib}")
endforeach()
list(APPEND NYTRIX_LLVM_LDFLAGS "-lstdc++")
# list(APPEND NYTRIX_LLVM_LDFLAGS "-lgcc_s") # Might be needed for some GCC versions/setups
