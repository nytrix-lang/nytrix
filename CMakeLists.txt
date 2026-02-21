cmake_minimum_required(VERSION 3.16)

project(nytrix C)
if (APPLE)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(NYTRIX_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(NYTRIX_BUILD_SHARED "Build shared runtime library" ON)
option(NYTRIX_BUILD_LSP "Build ny-lsp" ON)
option(NYTRIX_RELEASE_DEBUG_INFO "Emit DWARF/CodeView info for Release builds" OFF)
option(NYTRIX_USE_ASAN "Enable AddressSanitizer and UBSanitizer" OFF)
option(NYTRIX_USE_GPROF "Enable gprof profiling (-pg)" OFF)
option(NYTRIX_FAST_BUILD "Optimize for current hardware (-march=native)" OFF)
option(NYTRIX_ENABLE_TEST_MODE "Enable runtime and comptime test mode" OFF)


if (NYTRIX_USE_ASAN OR NYTRIX_USE_GPROF)
  if (NOT MSVC)
    add_compile_options(-fno-omit-frame-pointer)
  endif()
endif()

if (NYTRIX_USE_ASAN)
  if (MSVC)
    add_compile_options(/fsanitize=address)
  else()
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

if (NYTRIX_USE_GPROF)
  if (NOT MSVC)
    add_compile_options(-pg)
    add_link_options(-pg)
  endif()
endif()

if (NYTRIX_FAST_BUILD AND NOT MSVC)
  add_compile_options(-march=native)
endif()

# LLVM
execute_process(COMMAND llvm-config --cflags OUTPUT_VARIABLE LLVM_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND llvm-config --ldflags --libs all --system-libs OUTPUT_VARIABLE LLVM_LDFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
# Clean LLVM_CFLAGS/LDFLAGS from newlines
string(REPLACE "\n" " " LLVM_CFLAGS "${LLVM_CFLAGS}")
string(REPLACE "\n" " " LLVM_LDFLAGS "${LLVM_LDFLAGS}")

set(NYTRIX_LLVM_CFLAGS "${LLVM_CFLAGS}" CACHE STRING "LLVM C flags list")
set(NYTRIX_LLVM_LDFLAGS "${LLVM_LDFLAGS}" CACHE STRING "LLVM linker flags list")
set(NYTRIX_LLVM_INCLUDE "" CACHE STRING "LLVM include root (directory that contains llvm-c/)")
function(nytrix_parse_flag_list out_var in_flags)
  if("${in_flags}" STREQUAL "")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  # When flags arrive from `make`, they are already semicolon-separated CMake
  # lists (e.g. -IC:/Program Files/...;-IC:/.../llvm-c). Running
  # separate_arguments() on those re-splits by spaces and breaks Windows paths.
  if("${in_flags}" MATCHES ";")
    set(_parsed ${in_flags})
  else()
    separate_arguments(_parsed NATIVE_COMMAND "${in_flags}")
  endif()
  set(${out_var} "${_parsed}" PARENT_SCOPE)
endfunction()

nytrix_parse_flag_list(NYTRIX_LLVM_CFLAGS_LIST "${NYTRIX_LLVM_CFLAGS}")
nytrix_parse_flag_list(NYTRIX_LLVM_LDFLAGS_LIST "${NYTRIX_LLVM_LDFLAGS}")

# Robust include-root resolution for LLVM C headers on Windows.
if (NOT NYTRIX_LLVM_INCLUDE AND DEFINED ENV{NYTRIX_LLVM_HEADERS})
  set(NYTRIX_LLVM_INCLUDE "$ENV{NYTRIX_LLVM_HEADERS}")
endif()
if (NOT NYTRIX_LLVM_INCLUDE AND DEFINED ENV{LLVM_ROOT})
  if (EXISTS "$ENV{LLVM_ROOT}/include/llvm-c/Core.h")
    set(NYTRIX_LLVM_INCLUDE "$ENV{LLVM_ROOT}/include")
  endif()
endif()
if (NOT NYTRIX_LLVM_INCLUDE)
  foreach(_f IN LISTS NYTRIX_LLVM_CFLAGS_LIST)
    if (_f MATCHES "^-I(.+)$")
      set(_cand "${CMAKE_MATCH_1}")
      if (EXISTS "${_cand}/llvm-c/Core.h")
        set(NYTRIX_LLVM_INCLUDE "${_cand}")
        break()
      endif()
      if (EXISTS "${_cand}/Core.h" AND EXISTS "${_cand}/../llvm-c/Core.h")
        set(NYTRIX_LLVM_INCLUDE "${_cand}/..")
        break()
      endif()
    endif()
  endforeach()
endif()
if (NYTRIX_LLVM_INCLUDE)
  file(TO_CMAKE_PATH "${NYTRIX_LLVM_INCLUDE}" NYTRIX_LLVM_INCLUDE)
  if (EXISTS "${NYTRIX_LLVM_INCLUDE}/Core.h" AND EXISTS "${NYTRIX_LLVM_INCLUDE}/../llvm-c/Core.h")
    set(NYTRIX_LLVM_INCLUDE "${NYTRIX_LLVM_INCLUDE}/..")
  endif()
endif()
set(NYTRIX_WINSDK_CFLAGS "" CACHE STRING "Windows SDK include flags list")
set(NYTRIX_WINSDK_LDFLAGS "" CACHE STRING "Windows SDK lib flags list")

set(NYTRIX_HOST_CFLAGS "" CACHE STRING "Host-specific C flags (semicolon-separated)")
set(NYTRIX_HOST_LDFLAGS "" CACHE STRING "Host-specific linker flags (semicolon-separated)")
set(NYTRIX_LTO_MODE "off" CACHE STRING "LTO mode: off|thin|full")
set_property(CACHE NYTRIX_LTO_MODE PROPERTY STRINGS off thin full)
set(NYTRIX_PGO_MODE "off" CACHE STRING "PGO mode: off|gen|use")
set_property(CACHE NYTRIX_PGO_MODE PROPERTY STRINGS off gen use)
set(NYTRIX_PGO_PROFILE "" CACHE STRING "Profile data path for PGO use mode")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(nytrix_apply_compile_perf_flags target_name)
  if (NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    return()
  endif()

  string(TOLOWER "${NYTRIX_LTO_MODE}" _ny_lto_mode)
  if (_ny_lto_mode STREQUAL "thin")
    target_compile_options(${target_name} PRIVATE -flto=thin)
  elseif(_ny_lto_mode STREQUAL "full")
    target_compile_options(${target_name} PRIVATE -flto)
  endif()

  string(TOLOWER "${NYTRIX_PGO_MODE}" _ny_pgo_mode)
  if (_ny_pgo_mode STREQUAL "gen")
    target_compile_options(${target_name} PRIVATE -fprofile-generate)
  elseif(_ny_pgo_mode STREQUAL "use" AND NYTRIX_PGO_PROFILE)
    target_compile_options(${target_name} PRIVATE "-fprofile-use=${NYTRIX_PGO_PROFILE}" -fprofile-correction)
  endif()
endfunction()

function(nytrix_apply_link_perf_flags target_name)
  if (NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    return()
  endif()

  string(TOLOWER "${NYTRIX_LTO_MODE}" _ny_lto_mode)
  if (_ny_lto_mode STREQUAL "thin")
    target_link_options(${target_name} PRIVATE -flto=thin)
    if (UNIX AND NOT APPLE AND NOT WIN32)
      target_link_options(${target_name} PRIVATE
        "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache"
        "-Wl,--thinlto-cache-policy,cache_size_bytes=10737418240")
    endif()
  elseif(_ny_lto_mode STREQUAL "full")
    target_link_options(${target_name} PRIVATE -flto)
  endif()

  string(TOLOWER "${NYTRIX_PGO_MODE}" _ny_pgo_mode)
  if (_ny_pgo_mode STREQUAL "gen")
    target_link_options(${target_name} PRIVATE -fprofile-generate)
  elseif(_ny_pgo_mode STREQUAL "use" AND NYTRIX_PGO_PROFILE)
    target_link_options(${target_name} PRIVATE "-fprofile-use=${NYTRIX_PGO_PROFILE}" -fprofile-correction)
  endif()
endfunction()

set(STD_BUNDLE "${CMAKE_BINARY_DIR}/std.ny")
set(STD_SYMBOLS "${CMAKE_BINARY_DIR}/std_symbols.h")
file(GLOB_RECURSE STD_SOURCES CONFIGURE_DEPENDS "${NYTRIX_ROOT}/std/*.ny")

add_custom_command(
  OUTPUT "${STD_BUNDLE}" "${STD_SYMBOLS}"
  COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
          "${Python3_EXECUTABLE}" -B "${NYTRIX_ROOT}/etc/tools/std.py" "${STD_BUNDLE}"
  WORKING_DIRECTORY "${NYTRIX_ROOT}"
  DEPENDS "${NYTRIX_ROOT}/etc/tools/std.py" ${STD_SOURCES}
  COMMENT "Bundling std"
)

add_custom_target(std_bundle ALL DEPENDS "${STD_BUNDLE}" "${STD_SYMBOLS}")

file(GLOB SRC_AST CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/ast/*.c")
file(GLOB SRC_BASE CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/base/*.c")
file(GLOB SRC_LEX CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/lex/*.c")
file(GLOB SRC_SEMA CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/sema/*.c")
file(GLOB SRC_CODE CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/code/*.c")
file(GLOB SRC_REPL CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/repl/*.c")
file(GLOB SRC_WIRE CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/wire/*.c")

set(SRC_COMPILER
  ${SRC_AST}
  ${SRC_BASE}
  ${SRC_LEX}
  ${SRC_SEMA}
  ${SRC_CODE}
  ${SRC_REPL}
  ${SRC_WIRE}
  "${NYTRIX_ROOT}/src/parse/shared.c"
)

set(RT_INIT "${NYTRIX_ROOT}/src/rt/init.c")
file(GLOB RT_SOURCES CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/rt/*.c")
file(GLOB RT_HEADERS CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/rt/*.h")
list(REMOVE_ITEM RT_SOURCES "${RT_INIT}")

# Runtime is compiled via src/rt/init.c (which #includes the other rt/*.c
# files). Mark all runtime sources/headers as dependencies so Ninja rebuilds
# ny/ny_debug when any runtime unit changes.
set_source_files_properties("${RT_INIT}" PROPERTIES
  OBJECT_DEPENDS "${RT_SOURCES};${RT_HEADERS}"
)

add_library(nytrix_compiler OBJECT ${SRC_COMPILER})
add_library(nytrix_runtime OBJECT "${RT_INIT}")

add_dependencies(nytrix_compiler std_bundle)
add_dependencies(nytrix_runtime std_bundle)
nytrix_apply_compile_perf_flags(nytrix_compiler)
nytrix_apply_compile_perf_flags(nytrix_runtime)

target_include_directories(nytrix_compiler PRIVATE
  "${NYTRIX_ROOT}/src"
  "${NYTRIX_ROOT}/src/base"
  "${NYTRIX_ROOT}/src/rt"
  "${CMAKE_BINARY_DIR}"
)

target_include_directories(nytrix_runtime PRIVATE
  "${NYTRIX_ROOT}/src"
  "${NYTRIX_ROOT}/src/base"
  "${NYTRIX_ROOT}/src/rt"
  "${CMAKE_BINARY_DIR}"
)

if (NYTRIX_LLVM_INCLUDE)
  target_include_directories(nytrix_compiler PRIVATE "${NYTRIX_LLVM_INCLUDE}")
  target_include_directories(nytrix_runtime PRIVATE "${NYTRIX_LLVM_INCLUDE}")
endif()

set(NYTRIX_STD_PATH "${CMAKE_INSTALL_PREFIX}/share/nytrix/std.ny")
string(REPLACE "\\" "/" NYTRIX_STD_PATH "${NYTRIX_STD_PATH}")

target_compile_definitions(nytrix_compiler PRIVATE
  __STDC_CONSTANT_MACROS
  __STDC_FORMAT_MACROS
  __STDC_LIMIT_MACROS
  VERBOSE_BUILD
  NYTRIX_STD_PATH="${NYTRIX_STD_PATH}"
)

target_compile_definitions(nytrix_runtime PRIVATE
  __STDC_CONSTANT_MACROS
  __STDC_FORMAT_MACROS
  __STDC_LIMIT_MACROS
  VERBOSE_BUILD
  NYTRIX_STD_PATH="${NYTRIX_STD_PATH}"
)

if (UNIX AND NOT APPLE)
  target_compile_definitions(nytrix_compiler PRIVATE _GNU_SOURCE)
  target_compile_definitions(nytrix_runtime PRIVATE _GNU_SOURCE)
endif()

if (WIN32)
  target_compile_definitions(nytrix_compiler PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
  target_compile_definitions(nytrix_runtime PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
endif()

if (NYTRIX_ENABLE_TEST_MODE)
  # Enable test-mode runtime toggles.
  target_compile_definitions(nytrix_runtime PRIVATE NYTRIX_TEST_MODE)
endif()

if (MSVC)
  add_compile_options(/W4)
elseif (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  set(NYTRIX_WARNING_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wstrict-prototypes
    -Wundef
    -Wwrite-strings
    -Wunused
    -Wno-cast-align
  )
  target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_WARNING_FLAGS})
  target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_WARNING_FLAGS})
  if (CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(NYTRIX_CLANG_WARNING_FLAGS
      -Wno-gnu-include-next
      -Wno-gnu-zero-variadic-macro-arguments
      -Wno-language-extension-token
    )
    target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_CLANG_WARNING_FLAGS})
    target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_CLANG_WARNING_FLAGS})
  endif()
  # Runtime and stdlib interop rely on low-level tag/pointer casts that are not
  # strict-aliasing friendly on all targets (notably ARM). Keep behavior
  # deterministic across architectures.
  target_compile_options(nytrix_compiler PRIVATE -fno-strict-aliasing)
  target_compile_options(nytrix_runtime PRIVATE -fno-strict-aliasing)
endif()

# Match previous debug/release behavior
# Optimization and frame pointers
if (MSVC)
  add_compile_options($<$<CONFIG:Debug>:/Od> $<$<CONFIG:Release>:/O2>)
else()
  add_compile_options($<$<CONFIG:Debug>:-O0> $<$<CONFIG:Release>:-O3>)
  add_compile_options(-fno-omit-frame-pointer)
endif()
add_compile_definitions($<$<CONFIG:Debug>:DEBUG>)
add_compile_definitions($<$<CONFIG:Release>:NDEBUG>)

# Debug information format
if (WIN32)
  # For Windows, generate CodeView debug info and PDB files
  add_compile_options(
    $<$<CONFIG:Debug>:-gcodeview>
  )
  if (NYTRIX_RELEASE_DEBUG_INFO)
    add_compile_options($<$<CONFIG:Release>:-gcodeview>)
  endif()

else()
  # For Unix-like systems, generate DWARF debug info
  add_compile_options(
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Debug>:-gdwarf>
  )
  if (NYTRIX_RELEASE_DEBUG_INFO)
    add_compile_options($<$<CONFIG:Release>:-g3>)
    add_compile_options($<$<CONFIG:Release>:-gdwarf>)
  endif()
endif()
if (WIN32)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
endif()

if (NYTRIX_LLVM_CFLAGS_LIST)
  target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_LLVM_CFLAGS_LIST})
  target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_LLVM_CFLAGS_LIST})
endif()

if (NYTRIX_HOST_CFLAGS)
  target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_HOST_CFLAGS})
  target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_HOST_CFLAGS})
endif()

if (WIN32 AND NYTRIX_WINSDK_CFLAGS)
  target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_WINSDK_CFLAGS})
  target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_WINSDK_CFLAGS})
endif()



add_executable(ny
  "${NYTRIX_ROOT}/src/cmd/ny/main.c"
  $<TARGET_OBJECTS:nytrix_compiler>
  $<TARGET_OBJECTS:nytrix_runtime>
)
add_dependencies(ny std_bundle)
target_include_directories(ny PRIVATE
  "${NYTRIX_ROOT}/src"
  "${NYTRIX_ROOT}/src/base"
  "${NYTRIX_ROOT}/src/rt"
  "${CMAKE_BINARY_DIR}"
)
if (NYTRIX_LLVM_INCLUDE)
  target_include_directories(ny PRIVATE "${NYTRIX_LLVM_INCLUDE}")
endif()
set_target_properties(ny PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  OUTPUT_NAME_DEBUG "ny_debug"
)
if (WIN32)
  # Set PDB properties for Windows builds
  set_target_properties(ny PROPERTIES
    PDB_NAME "$<TARGET_FILE_BASE_NAME:ny>.pdb"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(ny PROPERTIES PDB_NAME "ny_debug.pdb")
  endif()
endif()
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set_target_properties(ny PROPERTIES OUTPUT_NAME "ny_debug")
endif()
if (WIN32)
  target_link_options(ny PRIVATE
    $<$<CONFIG:Debug>:-Xlinker /DEBUG>
    $<$<CONFIG:Release>:-Xlinker /DEBUG>
  )
elseif(APPLE)
  target_compile_options(ny PRIVATE -fPIE)
  target_link_options(ny PRIVATE -Wl,-pie)
endif()
nytrix_apply_compile_perf_flags(ny)
nytrix_apply_link_perf_flags(ny)
if (UNIX AND NOT APPLE)
  target_compile_definitions(ny PRIVATE _GNU_SOURCE)
endif()

if (NYTRIX_BUILD_LSP)
  add_executable(ny-lsp
    "${NYTRIX_ROOT}/src/cmd/ny-lsp/main.c"
    $<TARGET_OBJECTS:nytrix_compiler>
    $<TARGET_OBJECTS:nytrix_runtime>
  )
  add_dependencies(ny-lsp std_bundle)
  target_include_directories(ny-lsp PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/base"
    "${NYTRIX_ROOT}/src/rt"
    "${CMAKE_BINARY_DIR}"
  )
  if (NYTRIX_LLVM_INCLUDE)
    target_include_directories(ny-lsp PRIVATE "${NYTRIX_LLVM_INCLUDE}")
  endif()
  set_target_properties(ny-lsp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )
  if (WIN32)
    # Set PDB properties for Windows builds
    set_target_properties(ny-lsp PROPERTIES
      PDB_NAME "$<TARGET_FILE_BASE_NAME:ny-lsp>.pdb"
      PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
      set_target_properties(ny-lsp PROPERTIES PDB_NAME "ny-lsp_debug.pdb")
    endif()
  endif()
  if (WIN32)
    target_link_options(ny-lsp PRIVATE
      $<$<CONFIG:Debug>:-Xlinker /DEBUG>
      $<$<CONFIG:Release>:-Xlinker /DEBUG>
    )
  endif()
  nytrix_apply_compile_perf_flags(ny-lsp)
  nytrix_apply_link_perf_flags(ny-lsp)
  if (UNIX AND NOT APPLE)
    target_compile_definitions(ny-lsp PRIVATE _GNU_SOURCE)
  endif()
endif()

if (NYTRIX_LLVM_CFLAGS_LIST)
  target_compile_options(ny PRIVATE ${NYTRIX_LLVM_CFLAGS_LIST})
  if (NYTRIX_BUILD_LSP)
    target_compile_options(ny-lsp PRIVATE ${NYTRIX_LLVM_CFLAGS_LIST})
  endif()
endif()

if (NYTRIX_HOST_CFLAGS)
  target_compile_options(ny PRIVATE ${NYTRIX_HOST_CFLAGS})
  if (NYTRIX_BUILD_LSP)
    target_compile_options(ny-lsp PRIVATE ${NYTRIX_HOST_CFLAGS})
  endif()
endif()

if (WIN32 AND NYTRIX_WINSDK_CFLAGS)
  target_compile_options(ny PRIVATE ${NYTRIX_WINSDK_CFLAGS})
  if (NYTRIX_BUILD_LSP)
    target_compile_options(ny-lsp PRIVATE ${NYTRIX_WINSDK_CFLAGS})
  endif()
endif()

if (NYTRIX_LLVM_LDFLAGS_LIST)
  target_link_options(ny PRIVATE ${NYTRIX_LLVM_LDFLAGS_LIST})
  if (NYTRIX_BUILD_LSP)
    target_link_options(ny-lsp PRIVATE ${NYTRIX_LLVM_LDFLAGS_LIST})
  endif()
endif()

if (NYTRIX_HOST_LDFLAGS)
  target_link_options(ny PRIVATE ${NYTRIX_HOST_LDFLAGS})
  if (NYTRIX_BUILD_LSP)
    target_link_options(ny-lsp PRIVATE ${NYTRIX_HOST_LDFLAGS})
  endif()
endif()

if (WIN32 AND NYTRIX_WINSDK_LDFLAGS)
  target_link_options(ny PRIVATE ${NYTRIX_WINSDK_LDFLAGS})
  if (NYTRIX_BUILD_LSP)
    target_link_options(ny-lsp PRIVATE ${NYTRIX_WINSDK_LDFLAGS})
  endif()
endif()

if (WIN32)
  target_compile_definitions(ny PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
  if (NYTRIX_BUILD_LSP)
    target_compile_definitions(ny-lsp PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
  endif()
  target_link_libraries(ny PRIVATE ws2_32 user32 kernel32 advapi32 shell32 ole32 uuid)
  if (NYTRIX_BUILD_LSP)
    target_link_libraries(ny-lsp PRIVATE ws2_32 user32 kernel32 advapi32 shell32 ole32 uuid)
  endif()
else()
  target_link_libraries(ny PRIVATE m pthread ${LLVM_LIBS})
  if (UNIX AND NOT APPLE)
    target_link_libraries(ny PRIVATE dl)
  endif()
  target_link_options(ny PRIVATE -rdynamic)
  if (NYTRIX_BUILD_LSP)
    target_link_libraries(ny-lsp PRIVATE m pthread ${LLVM_LIBS})
    if (UNIX AND NOT APPLE)
      target_link_libraries(ny-lsp PRIVATE dl)
    endif()
    target_link_options(ny-lsp PRIVATE -rdynamic)
  endif()
endif()

if (NYTRIX_BUILD_SHARED AND NOT WIN32)
  add_library(nytrixrt SHARED "${RT_INIT}")
  add_dependencies(nytrixrt std_bundle)
  nytrix_apply_compile_perf_flags(nytrixrt)
  nytrix_apply_link_perf_flags(nytrixrt)
  target_include_directories(nytrixrt PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/base"
    "${NYTRIX_ROOT}/src/rt"
    "${CMAKE_BINARY_DIR}"
  )
  target_compile_definitions(nytrixrt PRIVATE
    __STDC_CONSTANT_MACROS
    __STDC_FORMAT_MACROS
    __STDC_LIMIT_MACROS
    VERBOSE_BUILD
    NYTRIX_STD_PATH="${NYTRIX_STD_PATH}"
  )
  if (UNIX AND NOT APPLE)
    target_compile_definitions(nytrixrt PRIVATE _GNU_SOURCE)
    target_link_libraries(nytrixrt PRIVATE dl)
  endif()
endif()

install(TARGETS ny
  RUNTIME DESTINATION bin
)
if (NYTRIX_BUILD_LSP)
  install(TARGETS ny-lsp
    RUNTIME DESTINATION bin
  )
endif()
if (NYTRIX_BUILD_SHARED AND NOT WIN32)
  install(TARGETS nytrixrt
    LIBRARY DESTINATION lib
  )
endif()
install(FILES "${STD_BUNDLE}" DESTINATION share/nytrix RENAME std.ny)
install(DIRECTORY "${NYTRIX_ROOT}/src" DESTINATION share/nytrix
  FILES_MATCHING PATTERN "*.h" PATTERN "*.c"
)
install(DIRECTORY "${CMAKE_BINARY_DIR}/docs" DESTINATION share/nytrix OPTIONAL)
if (UNIX)
  install(DIRECTORY "${CMAKE_BINARY_DIR}/man" DESTINATION share/man OPTIONAL)
endif()
