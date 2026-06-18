cmake_minimum_required(VERSION 3.16)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

project(nytrix C CXX)
if (APPLE)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
include(CheckLinkerFlag)
include(CheckCSourceCompiles)

set(NYTRIX_ROOT "${CMAKE_CURRENT_LIST_DIR}")

add_compile_definitions(_GNU_SOURCE)

option(NYTRIX_BUILD_SHARED "Build shared runtime library" ON)
option(NYTRIX_BUILD_LSP "Build ny-lsp" ON)
option(NYTRIX_STRICT_WARNINGS "Enable strict warning policy (pedantic + shadow + truncation diagnostics)" OFF)
option(NYTRIX_RELEASE_DEBUG_INFO "Emit DWARF/CodeView info for Release builds" OFF)
option(NYTRIX_USE_ASAN "Enable AddressSanitizer and UBSanitizer" OFF)
option(NYTRIX_USE_GPROF "Enable gprof profiling (-pg)" OFF)
option(NYTRIX_FAST_BUILD "Optimize for current hardware (-march=native)" OFF)
option(NYTRIX_RUNTIME_O3 "Build runtime hot paths with -O3 in Release builds" ON)
option(NYTRIX_ENABLE_TEST_MODE "Enable runtime and comptime test mode" OFF)
option(NYTRIX_DEBUG_FRIENDLY "Enable debugger-friendly compile and link flags" ON)
option(NYTRIX_SPLIT_DWARF "Emit split DWARF debug info where supported" OFF)
set(NYTRIX_DWARF_VERSION "5" CACHE STRING "DWARF version for Unix-like debug builds")
set_property(CACHE NYTRIX_DWARF_VERSION PROPERTY STRINGS 2 3 4 5)

set(NYTRIX_VERSION_MAJOR "0")
set(NYTRIX_VERSION_MINOR "6")
set(NYTRIX_VERSION_PATCH "0")
set(NYTRIX_VERSION_REF "" CACHE STRING
    "Git ref used for version metadata; empty prefers the upstream remote ref")
set(NYTRIX_VERSION_HEADER "${CMAKE_BINARY_DIR}/nytrix_version.h")
set(NYTRIX_VERSION_STAMP "${CMAKE_BINARY_DIR}/nytrix_version.stamp")
set(NYTRIX_VERSION_GENERATOR "${CMAKE_BINARY_DIR}/nytrix_version_generate.cmake")

function(nytrix_git_state_deps out_var)
  set(_deps)
  set(_git_file "${NYTRIX_ROOT}/.git")
  set(_git_dir "${_git_file}")

  if(EXISTS "${_git_file}" AND NOT IS_DIRECTORY "${_git_file}")
    list(APPEND _deps "${_git_file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_git_file}")
    file(READ "${_git_file}" _git_link LIMIT 1024)
    if(_git_link MATCHES "gitdir: ([^\r\n]+)")
      set(_git_dir "${CMAKE_MATCH_1}")
      if(NOT IS_ABSOLUTE "${_git_dir}")
        get_filename_component(_git_dir "${NYTRIX_ROOT}/${_git_dir}" ABSOLUTE)
      endif()
    endif()
  endif()

  if(IS_DIRECTORY "${_git_dir}")
    set(_head "${_git_dir}/HEAD")
    if(EXISTS "${_head}")
      list(APPEND _deps "${_head}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_head}")
      file(READ "${_head}" _head_text LIMIT 1024)
      if(_head_text MATCHES "^ref: ([^\r\n]+)")
        set(_ref_log "${_git_dir}/logs/${CMAKE_MATCH_1}")
        if(EXISTS "${_ref_log}")
          list(APPEND _deps "${_ref_log}")
        endif()
      endif()
    endif()
    if(EXISTS "${_git_dir}/logs/HEAD")
      list(APPEND _deps "${_git_dir}/logs/HEAD")
    endif()
    if(EXISTS "${_git_dir}/packed-refs")
      list(APPEND _deps "${_git_dir}/packed-refs")
    endif()
    file(GLOB _remote_logs
      "${_git_dir}/logs/refs/remotes/*"
      "${_git_dir}/logs/refs/remotes/*/*")
    if(_remote_logs)
      list(APPEND _deps ${_remote_logs})
    endif()
  endif()

  if(_deps)
    list(REMOVE_DUPLICATES _deps)
  endif()
  set(${out_var} "${_deps}" PARENT_SCOPE)
endfunction()

nytrix_git_state_deps(NYTRIX_VERSION_GIT_DEPS)

set(_nytrix_version_generator [=[
set(_version_major "@NYTRIX_VERSION_MAJOR@")
set(_version_minor "@NYTRIX_VERSION_MINOR@")
set(_version_patch "@NYTRIX_VERSION_PATCH@")
set(_root "@NYTRIX_ROOT@")
set(_out "@NYTRIX_VERSION_HEADER@")
set(_version_ref "@NYTRIX_VERSION_REF@")

if(NOT _version_major)
  set(_version_major "0")
endif()
if(NOT _version_minor)
  set(_version_minor "6")
endif()
if(NOT _version_patch)
  set(_version_patch "0")
endif()

set(_patch "${_version_patch}")
set(_build "0")
set(_commit "source")
set(_source "source")
set(_dirty 0)
set(_from_git 0)

find_program(GIT_EXECUTABLE NAMES git)

function(nytrix_try_git_ref repo ref source_name)
  if(NOT GIT_EXECUTABLE OR NOT EXISTS "${repo}")
    return()
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${repo}" rev-list --count "${ref}"
    OUTPUT_VARIABLE _count
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${repo}" rev-parse --short=12 "${ref}"
    OUTPUT_VARIABLE _hash
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(_count MATCHES "^[0-9]+$" AND _hash)
    set(_build "${_count}" PARENT_SCOPE)
    set(_commit "${_hash}" PARENT_SCOPE)
    set(_source "${source_name}" PARENT_SCOPE)
    set(_from_git 1 PARENT_SCOPE)
  endif()
endfunction()

if(GIT_EXECUTABLE AND EXISTS "${_root}/.git")
  if(_version_ref)
    nytrix_try_git_ref("${_root}" "${_version_ref}" "git")
  endif()
  if(NOT _from_git)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${_root}" rev-parse --abbrev-ref --symbolic-full-name "@{u}"
      OUTPUT_VARIABLE _upstream_ref
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_upstream_ref)
      nytrix_try_git_ref("${_root}" "${_upstream_ref}" "git")
    endif()
  endif()
  if(NOT _from_git)
    nytrix_try_git_ref("${_root}" "origin/HEAD" "git")
  endif()
  if(NOT _from_git)
    nytrix_try_git_ref("${_root}" "origin/main" "git")
  endif()
  if(NOT _from_git)
    nytrix_try_git_ref("${_root}" "HEAD" "git")
  endif()
endif()

set(_metadata "source")
if(_from_git)
  set(_metadata "${_source}.${_commit}")
endif()
set(_version "${_version_major}.${_version_minor}.${_patch}+${_metadata}")
set(_tmp "${_out}.tmp")

file(WRITE "${_tmp}"
"#pragma once

#define NYTRIX_VERSION_MAJOR ${_version_major}
#define NYTRIX_VERSION_MINOR ${_version_minor}
#define NYTRIX_VERSION_PATCH ${_patch}
#define NYTRIX_VERSION_BUILD ${_build}
#define NYTRIX_VERSION_COMMIT \"${_commit}\"
#define NYTRIX_VERSION_SOURCE \"${_source}\"
#define NYTRIX_VERSION_DIRTY ${_dirty}
#define NYTRIX_VERSION \"${_version}\"
")

if(EXISTS "${_out}")
  file(READ "${_out}" _old)
else()
  set(_old "")
endif()
file(READ "${_tmp}" _new)
if(NOT _old STREQUAL _new)
  file(RENAME "${_tmp}" "${_out}")
else()
  file(REMOVE "${_tmp}")
endif()
]=])
string(CONFIGURE "${_nytrix_version_generator}" _nytrix_version_generator @ONLY)
if(EXISTS "${NYTRIX_VERSION_GENERATOR}")
  file(READ "${NYTRIX_VERSION_GENERATOR}" _nytrix_version_generator_old)
else()
  set(_nytrix_version_generator_old "")
endif()
if(NOT _nytrix_version_generator_old STREQUAL _nytrix_version_generator)
  file(WRITE "${NYTRIX_VERSION_GENERATOR}" "${_nytrix_version_generator}")
endif()
add_custom_command(
  OUTPUT "${NYTRIX_VERSION_STAMP}"
  COMMAND ${CMAKE_COMMAND}
          -P "${NYTRIX_VERSION_GENERATOR}"
  COMMAND ${CMAKE_COMMAND}
          -E touch "${NYTRIX_VERSION_STAMP}"
  DEPENDS "${NYTRIX_VERSION_GENERATOR}" ${NYTRIX_VERSION_GIT_DEPS}
  BYPRODUCTS "${NYTRIX_VERSION_HEADER}"
  COMMENT "Generating Git-derived version header"
  VERBATIM
)
add_custom_target(nytrix_version_header ALL
  DEPENDS "${NYTRIX_VERSION_STAMP}" "${NYTRIX_VERSION_HEADER}"
)
set_source_files_properties("${NYTRIX_VERSION_HEADER}" PROPERTIES GENERATED TRUE)


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
function(nytrix_parse_flag_list out_var in_flags)
  if("${in_flags}" STREQUAL "")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  # Handle semicolon-separated lists vs space-separated string
  if("${in_flags}" MATCHES ";")
    # Quote the expansion to preserve elements with spaces
    set(_parsed "${in_flags}")
  else()
    # Replace newlines with spaces first
    string(REPLACE "\n" " " _clean_flags "${in_flags}")
    separate_arguments(_parsed NATIVE_COMMAND "${_clean_flags}")
  endif()
  set(${out_var} "${_parsed}" PARENT_SCOPE)
endfunction()

if (NOT CMAKE_TOOLCHAIN_FILE)
  unset(NYTRIX_LLVM_INCLUDE)
  if (DEFINED ENV{NYTRIX_LLVM_INCLUDE})
    set(NYTRIX_LLVM_INCLUDE "$ENV{NYTRIX_LLVM_INCLUDE}" CACHE STRING "LLVM include root")
    set(NYTRIX_LLVM_INCLUDE "$ENV{NYTRIX_LLVM_INCLUDE}")
  endif()

  # Attempt to use llvm-config. Prefer an explicit LLVM_CONFIG env override,
  # then versioned binaries, because CI images can have an older generic
  # llvm-config on PATH while the dependency installer just added a newer LLVM.
  if (DEFINED ENV{LLVM_CONFIG} AND EXISTS "$ENV{LLVM_CONFIG}")
    set(LLVM_CONFIG_EXE "$ENV{LLVM_CONFIG}")
  elseif(APPLE AND EXISTS "/opt/homebrew/opt/llvm@20/bin/llvm-config")
    set(LLVM_CONFIG_EXE "/opt/homebrew/opt/llvm@20/bin/llvm-config")
  elseif(APPLE AND EXISTS "/usr/local/opt/llvm@20/bin/llvm-config")
    set(LLVM_CONFIG_EXE "/usr/local/opt/llvm@20/bin/llvm-config")
  else()
    find_program(LLVM_CONFIG_EXE
                 NAMES llvm-config-22 llvm-config-21 llvm-config-20 llvm-config-19
                       llvm-config-18 llvm-config-17 llvm-config-16 llvm-config
                 PATHS "$ENV{MSYSTEM_PREFIX}/bin" "$ENV{MINGW_PREFIX}/bin"
                       "C:/msys64/ucrt64/bin" "C:/msys64/clang64/bin"
                       "C:/msys64/mingw64/bin" "C:/msys64/clangarm64/bin"
                       "C:/tools/msys64/ucrt64/bin"
                       "C:/tools/msys64/clang64/bin" "C:/tools/msys64/mingw64/bin"
                       "C:/tools/msys64/clangarm64/bin"
                       "C:/Program Files/LLVM/bin"
                       "C:/tools/llvm/bin" "/opt/homebrew/opt/llvm@20/bin"
                       "/usr/local/opt/llvm@20/bin" "/opt/homebrew/opt/llvm/bin"
                       "/usr/local/opt/llvm/bin")
  endif()
  if (LLVM_CONFIG_EXE)
    message(STATUS "Found llvm-config: ${LLVM_CONFIG_EXE}")
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --version OUTPUT_VARIABLE LLVM_VERSION_RAW OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --cflags OUTPUT_VARIABLE LLVM_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --ldflags OUTPUT_VARIABLE LLVM_LDFLAGS_RAW OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --libs all --system-libs OUTPUT_VARIABLE LLVM_LIBS_RAW OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LLVM_CONFIG_EXE} --libdir OUTPUT_VARIABLE LLVM_LIBDIR_RAW OUTPUT_STRIP_TRAILING_WHITESPACE)

    if (NOT NYTRIX_LLVM_INCLUDE)
      execute_process(COMMAND ${LLVM_CONFIG_EXE} --includedir OUTPUT_VARIABLE NYTRIX_LLVM_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()

    set(NYTRIX_LLVM_CFLAGS "${LLVM_CFLAGS}")
    set(NYTRIX_LLVM_LDFLAGS "${LLVM_LDFLAGS_RAW}")
    set(NYTRIX_LLVM_LIBDIR "${LLVM_LIBDIR_RAW}")
    if (LLVM_VERSION_RAW MATCHES "^([0-9]+)")
      set(NYTRIX_LLVM_VERSION_MAJOR "${CMAKE_MATCH_1}")
      if (NYTRIX_LLVM_VERSION_MAJOR VERSION_LESS 16 OR
          NYTRIX_LLVM_VERSION_MAJOR VERSION_GREATER 22)
        message(FATAL_ERROR
          "Unsupported LLVM ${LLVM_VERSION_RAW}. Nytrix supports LLVM 16 through 22; "
          "set LLVM_CONFIG to a supported llvm-config binary.")
      endif()
    endif()
    nytrix_parse_flag_list(LLVM_LIBS "${LLVM_LIBS_RAW}")
  else()
    message(WARNING "llvm-config not found. Using environment variables if available.")
  endif()

  if (DEFINED ENV{NYTRIX_LLVM_INCLUDE} AND NOT NYTRIX_LLVM_INCLUDE)
     set(NYTRIX_LLVM_INCLUDE "$ENV{NYTRIX_LLVM_INCLUDE}")
  endif()
  if (DEFINED ENV{NYTRIX_LLVM_HEADERS} AND NOT NYTRIX_LLVM_INCLUDE)
     set(NYTRIX_LLVM_INCLUDE "$ENV{NYTRIX_LLVM_HEADERS}")
  endif()

  if (WIN32 AND NOT NYTRIX_LLVM_INCLUDE)
     set(_llvm_search_paths
       "C:/Program Files/LLVM/include"
       "C:/msys64/ucrt64/include"
       "C:/msys64/clang64/include"
       "C:/msys64/mingw64/include"
       "C:/msys64/clangarm64/include"
       "C:/tools/msys64/ucrt64/include"
       "C:/tools/msys64/clang64/include"
       "C:/tools/msys64/mingw64/include"
       "C:/tools/msys64/clangarm64/include"
       "$ENV{MSYSTEM_PREFIX}/include"
       "$ENV{MINGW_PREFIX}/include"
       "C:/tools/llvm/include"
       "C:/Program Files (x86)/LLVM/include"
       "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/include"
       "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/Llvm/x64/include"
       "C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/x64/include"
     )
     foreach(_path IN LISTS _llvm_search_paths)
       if (EXISTS "${_path}/llvm-c/Core.h")
         set(NYTRIX_LLVM_INCLUDE "${_path}")
         break()
       endif()
     endforeach()
  endif()
  
  if (NYTRIX_LLVM_INCLUDE)
    file(TO_CMAKE_PATH "${NYTRIX_LLVM_INCLUDE}" NYTRIX_LLVM_INCLUDE)
    if (NOT EXISTS "${NYTRIX_LLVM_INCLUDE}/llvm-c/Core.h")
      message(WARNING "LLVM headers not found at: ${NYTRIX_LLVM_INCLUDE}/llvm-c/Core.h")
      set(NYTRIX_LLVM_INCLUDE "")
    elseif(NOT EXISTS "${NYTRIX_LLVM_INCLUDE}/clang-c/Index.h")
      message(WARNING "Clang C headers not found at: ${NYTRIX_LLVM_INCLUDE}/clang-c/Index.h")
      set(NYTRIX_LLVM_INCLUDE "")
    endif()
  endif()

  if (WIN32 AND NOT NYTRIX_LLVM_INCLUDE)
    message(FATAL_ERROR "LLVM/Clang include directory not found! Verified standard paths and ENV{NYTRIX_LLVM_INCLUDE}. Please install LLVM+libclang or set NYTRIX_LLVM_INCLUDE to the include directory containing llvm-c/Core.h and clang-c/Index.h.")
  endif()
endif()

if (NOT NYTRIX_LLVM_INCLUDE)
  message(FATAL_ERROR "LLVM/Clang development headers are required (missing llvm-c/Core.h or clang-c/Index.h). Install llvm-dev and libclang-dev, or set NYTRIX_LLVM_INCLUDE.")
endif()

message(STATUS "Final NYTRIX_LLVM_INCLUDE: '${NYTRIX_LLVM_INCLUDE}'")

nytrix_parse_flag_list(NYTRIX_LLVM_CFLAGS_LIST "${NYTRIX_LLVM_CFLAGS}")
# For LDFLAGS, we need to be careful not to split paths incorrectly, but separate_arguments usually handles NATIVE_COMMAND well.
nytrix_parse_flag_list(NYTRIX_LLVM_LDFLAGS_LIST "${NYTRIX_LLVM_LDFLAGS}")

if (NYTRIX_LLVM_INCLUDE)
  file(TO_CMAKE_PATH "${NYTRIX_LLVM_INCLUDE}" NYTRIX_LLVM_INCLUDE)
  string(STRIP "${NYTRIX_LLVM_INCLUDE}" NYTRIX_LLVM_INCLUDE)
  set(NYTRIX_LLVM_INCLUDE "${NYTRIX_LLVM_INCLUDE}" CACHE STRING "LLVM include root" FORCE)
  include_directories(SYSTEM "${NYTRIX_LLVM_INCLUDE}")
endif()

set(_nytrix_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
set(CMAKE_REQUIRED_INCLUDES "${NYTRIX_LLVM_INCLUDE}")
check_c_source_compiles("
#include <llvm-c/Core.h>
int main(void) { return (int)LLVMConstantPtrAuthValueKind; }
" NYTRIX_HAS_LLVM_CONSTANT_PTR_AUTH_VALUE_KIND)
set(CMAKE_REQUIRED_INCLUDES "${_nytrix_saved_required_includes}")

set(NYTRIX_CLANG_LIBRARY "" CACHE FILEPATH "libclang library")
set(_nytrix_llvm_lib_paths)
if (NYTRIX_LLVM_LIBDIR)
  list(APPEND _nytrix_llvm_lib_paths "${NYTRIX_LLVM_LIBDIR}")
endif()
if (NYTRIX_LLVM_INCLUDE)
  get_filename_component(_nytrix_llvm_prefix "${NYTRIX_LLVM_INCLUDE}" DIRECTORY)
  list(APPEND _nytrix_llvm_lib_paths "${_nytrix_llvm_prefix}/lib")
endif()
list(APPEND _nytrix_llvm_lib_paths
  "/opt/homebrew/opt/llvm@20/lib"
  "/usr/local/opt/llvm@20/lib"
  "/opt/homebrew/opt/llvm/lib"
  "/usr/local/opt/llvm/lib"
  "/usr/lib/llvm-22/lib"
  "/usr/lib/llvm22/lib"
  "/usr/lib/llvm-21/lib"
  "/usr/lib/llvm21/lib"
  "/usr/lib/llvm-20/lib"
  "/usr/lib/llvm20/lib"
  "/usr/lib/llvm-19/lib"
  "/usr/lib/llvm19/lib"
  "/usr/lib/llvm-18/lib"
  "/usr/lib/llvm18/lib"
  "/usr/lib/llvm-17/lib"
  "/usr/lib/llvm17/lib"
  "C:/msys64/ucrt64/lib"
  "C:/msys64/clang64/lib"
  "C:/msys64/mingw64/lib"
  "C:/msys64/clangarm64/lib"
  "C:/tools/msys64/ucrt64/lib"
  "C:/tools/msys64/clang64/lib"
  "C:/tools/msys64/mingw64/lib"
  "C:/tools/msys64/clangarm64/lib"
  "C:/Program Files/LLVM/lib"
  "$ENV{MSYSTEM_PREFIX}/lib"
  "$ENV{MINGW_PREFIX}/lib"
)
if (NYTRIX_CLANG_LIBRARY AND EXISTS "${NYTRIX_CLANG_LIBRARY}")
  set(NYTRIX_CLANG_LIBRARY_FOUND "${NYTRIX_CLANG_LIBRARY}")
else()
  unset(NYTRIX_CLANG_LIBRARY_FOUND CACHE)
  set(_nytrix_clang_names clang libclang libclang.dll.a clang.dll.a)
  if (NYTRIX_LLVM_VERSION_MAJOR)
    list(PREPEND _nytrix_clang_names
      "clang-${NYTRIX_LLVM_VERSION_MAJOR}"
      "libclang-${NYTRIX_LLVM_VERSION_MAJOR}"
    )
  endif()
  find_library(NYTRIX_CLANG_LIBRARY_FOUND
    NAMES ${_nytrix_clang_names}
    HINTS "${NYTRIX_CLANG_LIBRARY}"
    PATHS ${_nytrix_llvm_lib_paths}
    NO_DEFAULT_PATH
  )
  if (NOT NYTRIX_CLANG_LIBRARY_FOUND)
    find_library(NYTRIX_CLANG_LIBRARY_FOUND
      NAMES ${_nytrix_clang_names}
      HINTS "${NYTRIX_CLANG_LIBRARY}"
      PATHS ${_nytrix_llvm_lib_paths}
    )
  endif()
endif()
if (NOT NYTRIX_CLANG_LIBRARY_FOUND)
  set(NYTRIX_CLANG_LIBRARY_FOUND clang)
endif()
message(STATUS "Final NYTRIX_CLANG_LIBRARY: '${NYTRIX_CLANG_LIBRARY_FOUND}'")

set(NYTRIX_GMP_INCLUDE "" CACHE PATH "GMP include directory")
set(NYTRIX_GMP_LIBRARY "" CACHE FILEPATH "GMP library")
if (DEFINED ENV{NYTRIX_GMP_INCLUDE} AND NOT NYTRIX_GMP_INCLUDE)
  set(NYTRIX_GMP_INCLUDE "$ENV{NYTRIX_GMP_INCLUDE}")
elseif(DEFINED ENV{GMP_INCLUDE_DIR} AND NOT NYTRIX_GMP_INCLUDE)
  set(NYTRIX_GMP_INCLUDE "$ENV{GMP_INCLUDE_DIR}")
endif()
if (DEFINED ENV{NYTRIX_GMP_LIBRARY} AND NOT NYTRIX_GMP_LIBRARY)
  set(NYTRIX_GMP_LIBRARY "$ENV{NYTRIX_GMP_LIBRARY}")
elseif(DEFINED ENV{GMP_LIBRARY} AND NOT NYTRIX_GMP_LIBRARY)
  set(NYTRIX_GMP_LIBRARY "$ENV{GMP_LIBRARY}")
endif()

find_path(NYTRIX_GMP_INCLUDE_DIR
  NAMES gmp.h
  HINTS "${NYTRIX_GMP_INCLUDE}"
  PATHS
    "/opt/homebrew/include"
    "/usr/local/include"
    "C:/vcpkg/installed/x64-windows/include"
    "C:/msys64/ucrt64/include"
    "C:/msys64/clang64/include"
    "C:/msys64/mingw64/include"
    "C:/msys64/clangarm64/include"
    "C:/tools/msys64/ucrt64/include"
    "C:/tools/msys64/clang64/include"
    "C:/tools/msys64/mingw64/include"
    "C:/tools/msys64/clangarm64/include"
    "$ENV{MSYSTEM_PREFIX}/include"
    "$ENV{MINGW_PREFIX}/include"
)
if (NYTRIX_GMP_LIBRARY AND EXISTS "${NYTRIX_GMP_LIBRARY}")
  set(NYTRIX_GMP_LIBRARY_FOUND "${NYTRIX_GMP_LIBRARY}")
else()
  find_library(NYTRIX_GMP_LIBRARY_FOUND
    NAMES gmp libgmp
    HINTS "${NYTRIX_GMP_LIBRARY}"
    PATHS
      "/opt/homebrew/lib"
      "/usr/local/lib"
      "C:/vcpkg/installed/x64-windows/lib"
      "C:/msys64/ucrt64/lib"
      "C:/msys64/clang64/lib"
      "C:/msys64/mingw64/lib"
      "C:/msys64/clangarm64/lib"
      "C:/tools/msys64/ucrt64/lib"
      "C:/tools/msys64/clang64/lib"
      "C:/tools/msys64/mingw64/lib"
      "C:/tools/msys64/clangarm64/lib"
      "$ENV{MSYSTEM_PREFIX}/lib"
      "$ENV{MINGW_PREFIX}/lib"
  )
endif()
if (NOT NYTRIX_GMP_INCLUDE_DIR OR NOT NYTRIX_GMP_LIBRARY_FOUND)
  message(FATAL_ERROR "GMP development files are required (missing gmp.h or libgmp). Install gmp/libgmp-dev or set NYTRIX_GMP_INCLUDE and NYTRIX_GMP_LIBRARY.")
endif()
message(STATUS "Found GMP include: '${NYTRIX_GMP_INCLUDE_DIR}'")
message(STATUS "Found GMP library: '${NYTRIX_GMP_LIBRARY_FOUND}'")

set(NYTRIX_WINSDK_CFLAGS "" CACHE STRING "Windows SDK include flags list")
set(NYTRIX_WINSDK_LDFLAGS "" CACHE STRING "Windows SDK lib flags list")

set(NYTRIX_HOST_CFLAGS "" CACHE STRING "Host-specific C flags (semicolon-separated)")
set(NYTRIX_HOST_LDFLAGS "" CACHE STRING "Host-specific linker flags (semicolon-separated)")
set(NYTRIX_LTO_MODE "off" CACHE STRING "LTO mode: off|thin|full")
set_property(CACHE NYTRIX_LTO_MODE PROPERTY STRINGS off thin full)
set(NYTRIX_PGO_MODE "off" CACHE STRING "PGO mode: off|gen|use")
set_property(CACHE NYTRIX_PGO_MODE PROPERTY STRINGS off gen use)
set(NYTRIX_PGO_PROFILE "" CACHE STRING "Profile data path for PGO use mode")
set(NYTRIX_ICF_MODE "off" CACHE STRING "ICF (Identical Code Folding) mode: off|safe|all")
set_property(CACHE NYTRIX_ICF_MODE PROPERTY STRINGS off safe all)
set(NYTRIX_ENABLE_BOLT "off" CACHE STRING "Enable BOLT post-link optimization: off|on")
set_property(CACHE NYTRIX_ENABLE_BOLT PROPERTY STRINGS off on)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(ZLIB REQUIRED)

set(NYTRIX_ENABLE_Z3 "on" CACHE STRING "Enable Z3-backed finite constraint solving: auto|on|off")
set_property(CACHE NYTRIX_ENABLE_Z3 PROPERTY STRINGS auto on off)
string(TOLOWER "${NYTRIX_ENABLE_Z3}" _nytrix_enable_z3_lc)
if (NOT _nytrix_enable_z3_lc MATCHES "^(auto|on|off)$")
  message(FATAL_ERROR "NYTRIX_ENABLE_Z3 must be auto, on, or off")
endif()
set(NYTRIX_Z3_FOUND FALSE)
if (NOT _nytrix_enable_z3_lc STREQUAL "off")
  find_package(PkgConfig QUIET)
  if (PkgConfig_FOUND)
    pkg_check_modules(NYTRIX_Z3_PC QUIET z3)
  endif()
  find_path(NYTRIX_Z3_INCLUDE_DIR
    NAMES z3.h
    HINTS ${NYTRIX_Z3_PC_INCLUDE_DIRS}
    PATHS
      "$ENV{MSYSTEM_PREFIX}/include"
      "$ENV{MINGW_PREFIX}/include"
      "/opt/homebrew/include"
      "/usr/local/include"
      "C:/vcpkg/installed/x64-windows/include"
  )
  find_library(NYTRIX_Z3_LIBRARY
    NAMES z3 libz3
    HINTS ${NYTRIX_Z3_PC_LIBRARY_DIRS}
    PATHS
      "$ENV{MSYSTEM_PREFIX}/lib"
      "$ENV{MINGW_PREFIX}/lib"
      "${NYTRIX_ROOT}/build/vendor/lib/host"
      "/opt/homebrew/lib"
      "/usr/local/lib"
      "C:/vcpkg/installed/x64-windows/lib"
  )
  if (NYTRIX_Z3_INCLUDE_DIR AND NYTRIX_Z3_LIBRARY)
    set(NYTRIX_Z3_FOUND TRUE)
    message(STATUS "Z3 enabled: ${NYTRIX_Z3_LIBRARY}")
  elseif (_nytrix_enable_z3_lc STREQUAL "on")
    message(FATAL_ERROR "NYTRIX_ENABLE_Z3=on but z3.h/libz3 were not found")
  else()
    message(STATUS "Z3 not found; using HM-only type solver")
  endif()
endif()


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

function(nytrix_apply_size_flags target_name)
  if (MSVC)
    return()
  endif()
  if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target_name} PRIVATE -ffunction-sections -fdata-sections)
    if (UNIX AND NOT APPLE AND NOT WIN32)
      target_link_options(${target_name} PRIVATE -Wl,--gc-sections)
    elseif(APPLE)
      target_link_options(${target_name} PRIVATE -Wl,-dead_strip)
    endif()
  endif()
endfunction()

set(STD_NY "${CMAKE_BINARY_DIR}/std.ny")
set(STD_SYMBOLS "${CMAKE_BINARY_DIR}/std_symbols.h")
file(GLOB_RECURSE STD_SOURCES_LIB CONFIGURE_DEPENDS "${NYTRIX_ROOT}/lib/*.ny")
set(STD_SOURCES ${STD_SOURCES_LIB})

option(NYTRIX_STD_VERBOSE "Verbose std.ny generation output" OFF)
set(_NYTRIX_VENDOR_LIB_DIR "${NYTRIX_ROOT}/build/vendor/lib/host")
if(UNIX AND NOT APPLE AND EXISTS "${_NYTRIX_VENDOR_LIB_DIR}")
  set(_NYTRIX_STD_ENV ${CMAKE_COMMAND} -E env "LD_LIBRARY_PATH=${_NYTRIX_VENDOR_LIB_DIR}:$ENV{LD_LIBRARY_PATH}")
elseif(APPLE AND EXISTS "${_NYTRIX_VENDOR_LIB_DIR}")
  set(_NYTRIX_STD_ENV ${CMAKE_COMMAND} -E env "DYLD_LIBRARY_PATH=${_NYTRIX_VENDOR_LIB_DIR}:$ENV{DYLD_LIBRARY_PATH}")
else()
  set(_NYTRIX_STD_ENV ${CMAKE_COMMAND} -E env)
endif()
if(NYTRIX_STD_VERBOSE)
  set(_NYTRIX_STD_VERBOSE_LEVEL 1)
else()
  set(_NYTRIX_STD_VERBOSE_LEVEL 0)
endif()

add_custom_command(
  OUTPUT "${STD_NY}" "${STD_SYMBOLS}"
  COMMAND ${_NYTRIX_STD_ENV} $<TARGET_FILE:ny> --bundle-std="${STD_NY}" --bundle-symbols="${STD_SYMBOLS}" --verbose=${_NYTRIX_STD_VERBOSE_LEVEL}
  WORKING_DIRECTORY "${NYTRIX_ROOT}"
  DEPENDS ny ${STD_SOURCES}
  COMMENT "Generating std.ny"
)

set(STD_BC "${CMAKE_BINARY_DIR}/std.bc")
add_custom_command(
  OUTPUT "${STD_BC}"
  COMMAND ${_NYTRIX_STD_ENV} $<TARGET_FILE:ny> --emit-bc="${STD_BC}" --emit-module=std "${STD_NY}"
  WORKING_DIRECTORY "${NYTRIX_ROOT}"
  DEPENDS ny "${STD_NY}"
  COMMENT "Pre-compiling stdlib bitcode"
)
add_custom_target(std ALL DEPENDS "${STD_NY}" "${STD_SYMBOLS}")
add_custom_target(std_bc ALL DEPENDS "${STD_BC}")

file(GLOB SRC_BASE CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/base/*.c")
file(GLOB SRC_SEMA CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/sema/*.c")
file(GLOB SRC_CODE CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/code/*.c")
file(GLOB SRC_REPL CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/repl/*.c")
file(GLOB SRC_WIRE CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/wire/*.c")

set(SRC_COMPILER
  ${SRC_BASE}
  ${SRC_SEMA}
  ${SRC_CODE}
  ${SRC_REPL}
  ${SRC_WIRE}
  "${NYTRIX_ROOT}/src/parse/ast.c"
  "${NYTRIX_ROOT}/src/parse/lexer.c"
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
set_property(TARGET nytrix_compiler PROPERTY POSITION_INDEPENDENT_CODE ON)
add_library(nytrix_runtime OBJECT "${RT_INIT}")
add_dependencies(nytrix_compiler nytrix_version_header)
add_dependencies(nytrix_runtime nytrix_version_header)

# nytrix_compiler and nytrix_runtime do not depend on generated std.ny at compile time.
nytrix_apply_compile_perf_flags(nytrix_compiler)
nytrix_apply_compile_perf_flags(nytrix_runtime)
nytrix_apply_size_flags(nytrix_compiler)
nytrix_apply_size_flags(nytrix_runtime)

target_include_directories(nytrix_compiler PRIVATE
  "${NYTRIX_ROOT}/src"
  "${NYTRIX_ROOT}/src/rt"
  "${CMAKE_BINARY_DIR}"
)

target_include_directories(nytrix_runtime PRIVATE
  "${NYTRIX_ROOT}/src"
  "${NYTRIX_ROOT}/src/rt"
  "${CMAKE_BINARY_DIR}"
)
target_include_directories(nytrix_runtime SYSTEM PRIVATE "${NYTRIX_GMP_INCLUDE_DIR}")

if (NYTRIX_LLVM_INCLUDE)
  target_include_directories(nytrix_compiler SYSTEM PRIVATE "${NYTRIX_LLVM_INCLUDE}")
  target_include_directories(nytrix_runtime SYSTEM PRIVATE "${NYTRIX_LLVM_INCLUDE}")
endif()
if (NYTRIX_Z3_FOUND)
  target_include_directories(nytrix_compiler SYSTEM PRIVATE "${NYTRIX_Z3_INCLUDE_DIR}")
  target_compile_definitions(nytrix_compiler PRIVATE NYTRIX_HAS_Z3=1)
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
if (NYTRIX_HAS_LLVM_CONSTANT_PTR_AUTH_VALUE_KIND)
  target_compile_definitions(nytrix_compiler PRIVATE
    NYTRIX_HAS_LLVM_CONSTANT_PTR_AUTH_VALUE_KIND=1
  )
endif()

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

if (WIN32 AND CMAKE_CROSSCOMPILING)
    # Create a directory for case-insensitive Windows headers in the build tree
    set(WIN_HEADERS_DIR "${CMAKE_BINARY_DIR}/win_headers")
    file(MAKE_DIRECTORY "${WIN_HEADERS_DIR}")

    if (EXISTS "/usr/x86_64-w64-mingw32/include/basetsd.h")
        # Create a symlink for BaseTsd.h (MinGW/Linux case)
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink
            "/usr/x86_64-w64-mingw32/include/basetsd.h"
            "${WIN_HEADERS_DIR}/BaseTsd.h"
        )
        target_include_directories(nytrix_compiler PRIVATE "${WIN_HEADERS_DIR}")
        target_include_directories(nytrix_runtime PRIVATE "${WIN_HEADERS_DIR}")
    endif()
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
    -Wunused
    -Wno-implicit-fallthrough
    -Wstrict-prototypes
    -Wundef
    -Wwrite-strings
    -Wno-unused-function
    -Wno-cast-align
  )
  if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND NYTRIX_WARNING_FLAGS
      -Wno-format-truncation
      -Wno-attribute-alias
      -Wno-clobbered
    )
  endif()
  if (NYTRIX_STRICT_WARNINGS)
    list(APPEND NYTRIX_WARNING_FLAGS -Wpedantic -Wshadow)
  endif()
  target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_WARNING_FLAGS})
  target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_WARNING_FLAGS})
  if (CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(NYTRIX_CLANG_WARNING_FLAGS
      -Wno-gnu-include-next
      -Wno-gnu-zero-variadic-macro-arguments
      -Wno-language-extension-token
      -Wno-unneeded-internal-declaration
    )
    target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_CLANG_WARNING_FLAGS})
    target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_CLANG_WARNING_FLAGS})
  endif()
  # Runtime and stdlib interop rely on low-level tag/pointer casts that are not
  # strict-aliasing friendly on all targets (notably ARM). Keep behavior
  # deterministic across architectures.
  target_compile_options(nytrix_compiler PRIVATE -fno-strict-aliasing)
  target_compile_options(nytrix_runtime PRIVATE -fno-strict-aliasing)
  if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_options(nytrix_compiler PRIVATE -fno-semantic-interposition -fno-plt)
    target_compile_options(nytrix_runtime PRIVATE -fno-semantic-interposition -fno-plt)
  endif()
endif()

# Match previous debug/release behavior
# Optimization and frame pointers
if (MSVC)
  add_compile_options($<$<CONFIG:Debug>:/Od> $<$<CONFIG:Release>:/O2>)
else()
  add_compile_options($<$<CONFIG:Debug>:-O0> $<$<CONFIG:Release>:-O2>)
  add_compile_options(-fno-omit-frame-pointer)
  if (NOT APPLE)
    find_program(MOLD_EXECUTABLE NAMES mold)
    if(MOLD_EXECUTABLE)
        message(STATUS "Found mold linker, using it for faster linking: ${MOLD_EXECUTABLE}")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=mold")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=mold")
    else()
        message(STATUS "mold linker not found, using default linker.")
    endif()
  endif()
endif()
if (NYTRIX_RUNTIME_O3 AND NOT MSVC)
  target_compile_options(nytrix_runtime PRIVATE $<$<CONFIG:Release>:-O3>)
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
  add_compile_options($<$<CONFIG:Debug>:-g3>)
  if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options($<$<CONFIG:Debug>:-gdwarf-${NYTRIX_DWARF_VERSION}>)
  else()
    add_compile_options($<$<CONFIG:Debug>:-gdwarf>)
  endif()
  if (NYTRIX_RELEASE_DEBUG_INFO)
    add_compile_options($<$<CONFIG:Release>:-g2>)
    if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      add_compile_options($<$<CONFIG:Release>:-gdwarf-${NYTRIX_DWARF_VERSION}>)
    else()
      add_compile_options($<$<CONFIG:Release>:-gdwarf>)
    endif()
  endif()
  if (NYTRIX_DEBUG_FRIENDLY AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
      -fno-omit-frame-pointer
      -fno-optimize-sibling-calls
    )
    if (NOT APPLE)
      add_link_options(-Wl,--build-id)
    endif()
    if (UNIX AND NOT APPLE)
      check_linker_flag(C "-Wl,--gdb-index" NYTRIX_LINKER_HAS_GDB_INDEX)
      if (NYTRIX_LINKER_HAS_GDB_INDEX)
        add_link_options(-Wl,--gdb-index)
      endif()
    endif()
    if (NYTRIX_SPLIT_DWARF AND UNIX AND NOT APPLE)
      add_compile_options(-gsplit-dwarf)
    endif()
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
  "${NYTRIX_ROOT}/src/cmd/ny/pkg.c"
  "${NYTRIX_ROOT}/src/cmd/fmt/fmt.c"
  "${NYTRIX_ROOT}/src/cmd/perf/perf.c"
  "${NYTRIX_ROOT}/src/cmd/test/test.c"
  "${NYTRIX_ROOT}/src/cmd/web/web.c"
  "${NYTRIX_ROOT}/src/cmd/make/make.c"
  $<TARGET_OBJECTS:nytrix_compiler>
  $<TARGET_OBJECTS:nytrix_runtime>
)
add_dependencies(ny nytrix_version_header)
# ny generates std.ny, so the compiler binary itself does not depend on it.
  target_include_directories(ny PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/cmd/tools"
    "${NYTRIX_ROOT}/src/rt"
    "${CMAKE_BINARY_DIR}"
  )
if (WIN32)
  target_include_directories(ny PRIVATE "${WIN_HEADERS_DIR}")
endif()
if (NYTRIX_LLVM_INCLUDE)
  target_include_directories(ny SYSTEM PRIVATE "${NYTRIX_LLVM_INCLUDE}")
endif()
if (NYTRIX_Z3_FOUND)
  target_link_libraries(ny PRIVATE "${NYTRIX_Z3_LIBRARY}")
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
if (MSVC)
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
nytrix_apply_size_flags(ny)
if (UNIX AND NOT APPLE)
  target_compile_definitions(ny PRIVATE _GNU_SOURCE)
  # ICF: Identical Code Folding — merge identical functions at link time
  string(TOLOWER "${NYTRIX_ICF_MODE}" _ny_icf_mode)
  if (_ny_icf_mode STREQUAL "safe")
    target_link_options(ny PRIVATE -Wl,--icf=safe)
  elseif(_ny_icf_mode STREQUAL "all")
    target_link_options(ny PRIVATE -Wl,--icf=all)
  endif()
endif()

function(nytrix_add_tool tool_target tool_name)
  set(_tool_sources
    "${NYTRIX_ROOT}/src/cmd/tools/main.c"
    "${NYTRIX_ROOT}/src/base/args.c"
  )
  if ("${tool_name}" STREQUAL "fmt")
    list(APPEND _tool_sources "${NYTRIX_ROOT}/src/cmd/fmt/fmt.c")
  elseif ("${tool_name}" STREQUAL "perf")
    list(APPEND _tool_sources "${NYTRIX_ROOT}/src/cmd/perf/perf.c")
  elseif ("${tool_name}" STREQUAL "test")
    list(APPEND _tool_sources "${NYTRIX_ROOT}/src/cmd/test/test.c")
  elseif ("${tool_name}" STREQUAL "doc")
    list(APPEND _tool_sources "${NYTRIX_ROOT}/src/cmd/web/web.c")
  elseif ("${tool_name}" STREQUAL "make")
    list(APPEND _tool_sources "${NYTRIX_ROOT}/src/cmd/make/make.c")
  endif()
  add_executable(${tool_target}
    ${_tool_sources}
  )
  add_dependencies(${tool_target} nytrix_version_header)
  target_compile_definitions(${tool_target} PRIVATE NY_TOOL_ENTRY="${tool_name}")
  if ("${tool_name}" STREQUAL "fmt")
    target_compile_definitions(${tool_target} PRIVATE NY_TOOL_HAS_FMT=1)
  elseif ("${tool_name}" STREQUAL "perf")
    target_compile_definitions(${tool_target} PRIVATE NY_TOOL_HAS_PERF=1)
  elseif ("${tool_name}" STREQUAL "test")
    target_compile_definitions(${tool_target} PRIVATE NY_TOOL_HAS_TEST=1)
  elseif ("${tool_name}" STREQUAL "doc")
    target_compile_definitions(${tool_target} PRIVATE NY_TOOL_HAS_WEB=1)
  elseif ("${tool_name}" STREQUAL "make")
    target_compile_definitions(${tool_target} PRIVATE NY_TOOL_HAS_MAKE=1)
  endif()
  target_include_directories(${tool_target} PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/rt"
    "${CMAKE_BINARY_DIR}"
  )
  if (MSVC)
    target_compile_options(${tool_target} PRIVATE /W4)
  elseif (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${tool_target} PRIVATE -Wall -Wextra -Wno-unused-function)
    if (NYTRIX_STRICT_WARNINGS)
      target_compile_options(${tool_target} PRIVATE -Wpedantic)
    endif()
  endif()
  set_target_properties(${tool_target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )
endfunction()

nytrix_add_tool(ny-fmt "fmt")
nytrix_add_tool(ny-perf "perf")
nytrix_add_tool(ny-test "test")
nytrix_add_tool(ny-doc "doc")
nytrix_add_tool(ny-make "make")

add_executable(ny-fuzz
  "${NYTRIX_ROOT}/src/cmd/fuzz/analyze.c"
  "${NYTRIX_ROOT}/src/cmd/fuzz/compare.c"
  "${NYTRIX_ROOT}/src/cmd/fuzz/generate.c"
  "${NYTRIX_ROOT}/src/cmd/fuzz/proc.c"
  "${NYTRIX_ROOT}/src/cmd/fuzz/shape.c"
  "${NYTRIX_ROOT}/src/cmd/fuzz/synth.c"
  "${NYTRIX_ROOT}/src/cmd/fuzz/usage.c"
  "${NYTRIX_ROOT}/src/cmd/fuzz/util.c"
  "${NYTRIX_ROOT}/src/cmd/fuzz/worker.c"
  "${NYTRIX_ROOT}/src/cmd/tools/cbridge.c"
)
add_dependencies(ny-fuzz nytrix_version_header)
target_include_directories(ny-fuzz PRIVATE
  "${NYTRIX_ROOT}/src"
  "${NYTRIX_ROOT}/src/cmd/tools"
  "${NYTRIX_ROOT}/src/rt"
  "${CMAKE_BINARY_DIR}"
)
set_target_properties(ny-fuzz PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if (NYTRIX_BUILD_LSP)
  add_executable(ny-lsp
    "${NYTRIX_ROOT}/src/cmd/lsp/main.c"
    $<TARGET_OBJECTS:nytrix_compiler>
    $<TARGET_OBJECTS:nytrix_runtime>
  )
  add_dependencies(ny-lsp nytrix_version_header)
  target_include_directories(ny-lsp PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/rt"
    "${CMAKE_BINARY_DIR}"
  )
  if (WIN32)
    target_include_directories(ny-lsp PRIVATE "${WIN_HEADERS_DIR}")
  endif()
  if (NYTRIX_LLVM_INCLUDE)
    target_include_directories(ny-lsp SYSTEM PRIVATE "${NYTRIX_LLVM_INCLUDE}")
  endif()
  if (NYTRIX_Z3_FOUND)
    target_link_libraries(ny-lsp PRIVATE "${NYTRIX_Z3_LIBRARY}")
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
  if (MSVC)
    target_link_options(ny-lsp PRIVATE
      $<$<CONFIG:Debug>:-Xlinker /DEBUG>
      $<$<CONFIG:Release>:-Xlinker /DEBUG>
    )
  endif()
  nytrix_apply_compile_perf_flags(ny-lsp)
  nytrix_apply_link_perf_flags(ny-lsp)
  nytrix_apply_size_flags(ny-lsp)
  if (UNIX AND NOT APPLE)
    target_compile_definitions(ny-lsp PRIVATE _GNU_SOURCE)
  endif()
endif()

add_executable(ny-dap
  "${NYTRIX_ROOT}/src/cmd/dap/main.c"
)
set_target_properties(ny-dap PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if (WIN32)
  set(NYTRIX_WINDOWS_RUNTIME_DEPS_SCRIPT "${CMAKE_BINARY_DIR}/nytrix_copy_runtime_deps.cmake")
  file(WRITE "${NYTRIX_WINDOWS_RUNTIME_DEPS_SCRIPT}" [=[
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()
if(NOT DEFINED NYTRIX_RUNTIME_EXE OR NOT EXISTS "${NYTRIX_RUNTIME_EXE}")
  return()
endif()
if(NOT DEFINED NYTRIX_RUNTIME_OUT)
  get_filename_component(NYTRIX_RUNTIME_OUT "${NYTRIX_RUNTIME_EXE}" DIRECTORY)
endif()
function(_nytrix_runtime_path_key out_var path)
  if(EXISTS "${path}")
    get_filename_component(_real "${path}" REALPATH)
  else()
    get_filename_component(_real "${path}" ABSOLUTE)
  endif()
  if(CMAKE_HOST_WIN32)
    string(TOLOWER "${_real}" _real)
  endif()
  set(${out_var} "${_real}" PARENT_SCOPE)
endfunction()
_nytrix_runtime_path_key(_nytrix_runtime_out_key "${NYTRIX_RUNTIME_OUT}")
set(_nytrix_runtime_dirs)
foreach(_dir IN LISTS NYTRIX_RUNTIME_DIRS)
  if(IS_DIRECTORY "${_dir}")
    list(APPEND _nytrix_runtime_dirs "${_dir}")
  endif()
endforeach()
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${NYTRIX_RUNTIME_EXE}"
  DIRECTORIES ${_nytrix_runtime_dirs}
  RESOLVED_DEPENDENCIES_VAR _nytrix_runtime_deps
  UNRESOLVED_DEPENDENCIES_VAR _nytrix_runtime_unresolved
  CONFLICTING_DEPENDENCIES_PREFIX _nytrix_runtime_conflicts
  PRE_EXCLUDE_REGEXES
    "api-ms-.*"
    "ext-ms-.*"
  POST_EXCLUDE_REGEXES
    "[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss]ystem32[/\\\\].*"
    "[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss]ysWOW64[/\\\\].*"
)
foreach(_name IN LISTS _nytrix_runtime_conflicts_FILENAMES)
  set(_conflict_var "_nytrix_runtime_conflicts_${_name}")
  set(_chosen)
  foreach(_path IN LISTS ${_conflict_var})
    get_filename_component(_dir "${_path}" DIRECTORY)
    _nytrix_runtime_path_key(_dir_key "${_dir}")
    if(NOT _dir_key STREQUAL _nytrix_runtime_out_key)
      set(_chosen "${_path}")
      break()
    endif()
  endforeach()
  if(NOT _chosen)
    foreach(_path IN LISTS ${_conflict_var})
      set(_chosen "${_path}")
      break()
    endforeach()
  endif()
  if(_chosen)
    list(APPEND _nytrix_runtime_deps "${_chosen}")
  endif()
endforeach()
if(_nytrix_runtime_deps)
  list(REMOVE_DUPLICATES _nytrix_runtime_deps)
endif()
foreach(_dep IN LISTS _nytrix_runtime_deps)
  get_filename_component(_name "${_dep}" NAME)
  set(_dst "${NYTRIX_RUNTIME_OUT}/${_name}")
  _nytrix_runtime_path_key(_src_key "${_dep}")
  _nytrix_runtime_path_key(_dst_key "${_dst}")
  if(NOT _src_key STREQUAL _dst_key)
    file(COPY_FILE "${_dep}" "${_dst}" ONLY_IF_DIFFERENT)
  endif()
endforeach()
if(_nytrix_runtime_unresolved)
  message(STATUS "Nytrix unresolved runtime deps for ${NYTRIX_RUNTIME_EXE}: ${_nytrix_runtime_unresolved}")
endif()
]=])

  function(nytrix_stage_windows_runtime_deps target_name)
    if(NOT TARGET ${target_name})
      return()
    endif()
    set(_runtime_dirs)
    foreach(_dir IN ITEMS
        "$ENV{MSYSTEM_PREFIX}/bin"
        "$ENV{MINGW_PREFIX}/bin"
        "C:/msys64/ucrt64/bin"
        "C:/msys64/clang64/bin"
        "C:/msys64/mingw64/bin"
        "C:/tools/msys64/ucrt64/bin"
        "C:/tools/msys64/clang64/bin"
        "C:/tools/msys64/mingw64/bin"
        "C:/Program Files/LLVM/bin")
      if(IS_DIRECTORY "${_dir}")
        list(APPEND _runtime_dirs "${_dir}")
      endif()
    endforeach()
    foreach(_path IN ITEMS
        "${CMAKE_C_COMPILER}"
        "${CMAKE_CXX_COMPILER}"
        "${NYTRIX_CLANG_LIBRARY_FOUND}"
        "${NYTRIX_GMP_LIBRARY_FOUND}"
        "${NYTRIX_Z3_LIBRARY}")
      if(_path)
        get_filename_component(_dir "${_path}" DIRECTORY)
        if(IS_DIRECTORY "${_dir}")
          list(APPEND _runtime_dirs "${_dir}")
        endif()
        get_filename_component(_parent "${_dir}" DIRECTORY)
        if(IS_DIRECTORY "${_parent}/bin")
          list(APPEND _runtime_dirs "${_parent}/bin")
        endif()
      endif()
    endforeach()
    if(TARGET ZLIB::ZLIB)
      get_target_property(_zlib_loc ZLIB::ZLIB IMPORTED_LOCATION)
      get_target_property(_zlib_implib ZLIB::ZLIB IMPORTED_IMPLIB)
      foreach(_path IN ITEMS "${_zlib_loc}" "${_zlib_implib}")
        if(_path)
          get_filename_component(_dir "${_path}" DIRECTORY)
          get_filename_component(_parent "${_dir}" DIRECTORY)
          if(IS_DIRECTORY "${_dir}")
            list(APPEND _runtime_dirs "${_dir}")
          endif()
          if(IS_DIRECTORY "${_parent}/bin")
            list(APPEND _runtime_dirs "${_parent}/bin")
          endif()
        endif()
      endforeach()
    endif()
    list(REMOVE_DUPLICATES _runtime_dirs)
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND "${CMAKE_COMMAND}"
        "-DNYTRIX_RUNTIME_EXE=$<TARGET_FILE:${target_name}>"
        "-DNYTRIX_RUNTIME_OUT=$<TARGET_FILE_DIR:${target_name}>"
        "-DNYTRIX_RUNTIME_DIRS=${_runtime_dirs}"
        -P "${NYTRIX_WINDOWS_RUNTIME_DEPS_SCRIPT}"
      VERBATIM
    )
  endfunction()

  target_compile_definitions(ny PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
  if (NYTRIX_BUILD_LSP)
    target_compile_definitions(ny-lsp PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
  endif()

  # CFLAGS
  set(_all_cflags ${NYTRIX_LLVM_CFLAGS_LIST} ${NYTRIX_HOST_CFLAGS} ${NYTRIX_WINSDK_CFLAGS})
  foreach(_flag IN LISTS _all_cflags)
    target_compile_options(ny PRIVATE "${_flag}")
    if (NYTRIX_BUILD_LSP)
      target_compile_options(ny-lsp PRIVATE "${_flag}")
    endif()
  endforeach()

  # LDFLAGS & LIBS
  target_link_libraries(ny PRIVATE ws2_32 user32 kernel32 advapi32 shell32 ole32 uuid "${NYTRIX_CLANG_LIBRARY_FOUND}" "${NYTRIX_GMP_LIBRARY_FOUND}" ZLIB::ZLIB)
  set(_all_ldflags ${NYTRIX_LLVM_LDFLAGS_LIST} ${NYTRIX_HOST_LDFLAGS} ${NYTRIX_WINSDK_LDFLAGS})
  foreach(_flag IN LISTS _all_ldflags)
    if(_flag MATCHES "^-L" OR _flag MATCHES "^/LIBPATH" OR _flag MATCHES "^-l")
      target_link_options(ny PRIVATE "${_flag}")
    else()
      target_link_libraries(ny PRIVATE "${_flag}")
    endif()
  endforeach()
  foreach(_lib IN LISTS LLVM_LIBS)
    target_link_libraries(ny PRIVATE "${_lib}")
  endforeach()

  if (NYTRIX_BUILD_LSP)
    target_link_libraries(ny-lsp PRIVATE ws2_32 user32 kernel32 advapi32 shell32 ole32 uuid "${NYTRIX_CLANG_LIBRARY_FOUND}" "${NYTRIX_GMP_LIBRARY_FOUND}" ZLIB::ZLIB)
    foreach(_flag IN LISTS _all_ldflags)
      if(_flag MATCHES "^-L" OR _flag MATCHES "^/LIBPATH" OR _flag MATCHES "^-l")
        target_link_options(ny-lsp PRIVATE "${_flag}")
      else()
        target_link_libraries(ny-lsp PRIVATE "${_flag}")
      endif()
    endforeach()
    foreach(_lib IN LISTS LLVM_LIBS)
      target_link_libraries(ny-lsp PRIVATE "${_lib}")
    endforeach()
  endif()
  foreach(_target IN ITEMS ny ny-fmt ny-perf ny-test ny-doc ny-make ny-lsp)
    nytrix_stage_windows_runtime_deps(${_target})
  endforeach()
else()
  # CFLAGS
  set(_all_cflags ${NYTRIX_LLVM_CFLAGS_LIST} ${NYTRIX_HOST_CFLAGS})
  foreach(_flag IN LISTS _all_cflags)
    target_compile_options(ny PRIVATE "${_flag}")
    if (NYTRIX_BUILD_LSP)
      target_compile_options(ny-lsp PRIVATE "${_flag}")
    endif()
  endforeach()

  # LDFLAGS & LIBS
  target_link_libraries(ny PRIVATE m pthread "${NYTRIX_CLANG_LIBRARY_FOUND}" "${NYTRIX_GMP_LIBRARY_FOUND}")
  set(_all_ldflags ${NYTRIX_LLVM_LDFLAGS_LIST} ${NYTRIX_HOST_LDFLAGS})
  foreach(_flag IN LISTS _all_ldflags)
    if(_flag MATCHES "^-L" OR _flag MATCHES "^-l")
      target_link_options(ny PRIVATE "${_flag}")
    else()
      target_link_libraries(ny PRIVATE "${_flag}")
    endif()
  endforeach()
  foreach(_lib IN LISTS LLVM_LIBS)
    target_link_libraries(ny PRIVATE "${_lib}")
  endforeach()

  if (UNIX AND NOT APPLE)
    target_link_libraries(ny PRIVATE dl util m ZLIB::ZLIB)
  elseif (APPLE)
    target_link_libraries(ny PRIVATE ZLIB::ZLIB)
  endif()
  target_link_options(ny PRIVATE -rdynamic)

  if (NYTRIX_BUILD_LSP)
    target_link_libraries(ny-lsp PRIVATE m pthread util "${NYTRIX_CLANG_LIBRARY_FOUND}" "${NYTRIX_GMP_LIBRARY_FOUND}")
    foreach(_flag IN LISTS _all_ldflags)
      if(_flag MATCHES "^-L" OR _flag MATCHES "^-l")
        target_link_options(ny-lsp PRIVATE "${_flag}")
      else()
        target_link_libraries(ny-lsp PRIVATE "${_flag}")
      endif()
    endforeach()
    foreach(_lib IN LISTS LLVM_LIBS)
      target_link_libraries(ny-lsp PRIVATE "${_lib}")
    endforeach()
    if (UNIX AND NOT APPLE)
      target_link_libraries(ny-lsp PRIVATE dl ZLIB::ZLIB)
    elseif (APPLE)
      target_link_libraries(ny-lsp PRIVATE ZLIB::ZLIB)
    endif()
  target_link_options(ny-lsp PRIVATE -rdynamic)
  endif()
endif()

if (NYTRIX_BUILD_SHARED AND NOT WIN32)
  add_library(nytrixrt SHARED "${RT_INIT}" $<TARGET_OBJECTS:nytrix_compiler>)
  add_dependencies(nytrixrt nytrix_version_header)
  add_dependencies(nytrixrt std)
  nytrix_apply_size_flags(nytrixrt)
  # CFLAGS
  set(_all_cflags ${NYTRIX_LLVM_CFLAGS_LIST} ${NYTRIX_HOST_CFLAGS})
  foreach(_flag IN LISTS _all_cflags)
    target_compile_options(nytrixrt PRIVATE "${_flag}")
  endforeach()

  target_include_directories(nytrixrt PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/rt"
    "${CMAKE_BINARY_DIR}"
  )
  target_include_directories(nytrixrt SYSTEM PRIVATE "${NYTRIX_GMP_INCLUDE_DIR}")
  if (NYTRIX_LLVM_INCLUDE)
    target_include_directories(nytrixrt PRIVATE "${NYTRIX_LLVM_INCLUDE}")
  endif()
  if (NYTRIX_Z3_FOUND)
    target_include_directories(nytrixrt SYSTEM PRIVATE "${NYTRIX_Z3_INCLUDE_DIR}")
    target_link_libraries(nytrixrt PRIVATE "${NYTRIX_Z3_LIBRARY}")
  endif()
  target_compile_definitions(nytrixrt PRIVATE
    __STDC_CONSTANT_MACROS
    __STDC_FORMAT_MACROS
    __STDC_LIMIT_MACROS
    VERBOSE_BUILD
    NYTRIX_STD_PATH="${NYTRIX_STD_PATH}"
  )

  # LDFLAGS & LIBS
  target_link_libraries(nytrixrt PRIVATE "${NYTRIX_GMP_LIBRARY_FOUND}")
  set(_all_ldflags ${NYTRIX_LLVM_LDFLAGS_LIST} ${NYTRIX_HOST_LDFLAGS})
  foreach(_flag IN LISTS _all_ldflags)
    if(_flag MATCHES "^-L" OR _flag MATCHES "^-l")
      target_link_options(nytrixrt PRIVATE "${_flag}")
    else()
      target_link_libraries(nytrixrt PRIVATE "${_flag}")
    endif()
  endforeach()
  foreach(_lib IN LISTS LLVM_LIBS)
    target_link_libraries(nytrixrt PRIVATE "${_lib}")
  endforeach()
  if (UNIX AND NOT APPLE)
    target_compile_definitions(nytrixrt PRIVATE _GNU_SOURCE)
    target_link_libraries(nytrixrt PRIVATE dl ZLIB::ZLIB)
  elseif (APPLE)
    target_link_libraries(nytrixrt PRIVATE ZLIB::ZLIB)
  endif()
endif()

install(TARGETS ny
  RUNTIME DESTINATION bin
)
option(NYTRIX_INSTALL_BIN_ALIAS
  "Install /bin/ny as a compatibility link to the installed ny executable"
  ON
)
if (UNIX AND NOT APPLE AND NYTRIX_INSTALL_BIN_ALIAS)
  install(CODE [[
    if (NOT DEFINED CMAKE_INSTALL_PREFIX OR CMAKE_INSTALL_PREFIX STREQUAL "")
      set(CMAKE_INSTALL_PREFIX "/usr/local")
    endif()
    set(_ny_bin_target "${CMAKE_INSTALL_PREFIX}/bin/ny")
    set(_ny_bin_link "$ENV{DESTDIR}/bin/ny")
    if (_ny_bin_target STREQUAL "/bin/ny")
      message(STATUS "Installing: ${_ny_bin_link}")
    else()
      file(MAKE_DIRECTORY "$ENV{DESTDIR}/bin")
      if (EXISTS "${_ny_bin_link}" OR IS_SYMLINK "${_ny_bin_link}")
        file(REMOVE "${_ny_bin_link}")
      endif()
      file(CREATE_LINK "${_ny_bin_target}" "${_ny_bin_link}" RESULT _ny_bin_alias_result SYMBOLIC)
      if (NOT _ny_bin_alias_result STREQUAL "0")
        message(FATAL_ERROR "failed to install ${_ny_bin_link}: ${_ny_bin_alias_result}")
      endif()
      message(STATUS "Installing: ${_ny_bin_link} -> ${_ny_bin_target}")
    endif()
  ]])
endif()
# Standalone tool targets remain buildable for developer compatibility.
# Installed tooling is exposed through `ny fmt|perf|test|doc|make|pkg`.
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
install(FILES "${STD_NY}" DESTINATION share/nytrix RENAME std.ny)
install(DIRECTORY "${NYTRIX_ROOT}/lib" DESTINATION share/nytrix
  FILES_MATCHING PATTERN "*.ny"
)
install(DIRECTORY "${NYTRIX_ROOT}/etc/assets" DESTINATION share/nytrix/etc)
install(DIRECTORY "${NYTRIX_ROOT}/src" DESTINATION share/nytrix
  FILES_MATCHING PATTERN "*.h" PATTERN "*.c"
)
install(DIRECTORY "${CMAKE_BINARY_DIR}/docs" DESTINATION share/nytrix OPTIONAL)
if (UNIX)
  install(DIRECTORY "${CMAKE_BINARY_DIR}/man" DESTINATION share/man OPTIONAL)
endif()

# BOLT post-link optimizer
# Runs after the final binary is linked. Requires perf data from a profiling run.
# Usage: NYTRIX_ENABLE_BOLT=on cmake ... then run `make bolt_profile && make bolt_ny`
if (NYTRIX_ENABLE_BOLT AND UNIX AND NOT APPLE)
  find_program(LLVM_BOLT NAMES llvm-bolt llvm-bolt-18 llvm-bolt-17 llvm-bolt-16)
  find_program(MERGE_FDATA NAMES merge-fdata merge-fdata-18 merge-fdata-17 merge-fdata-16)
  find_program(PERF NAMES perf)
  if (LLVM_BOLT AND PERF)
    message(STATUS "BOLT enabled: ${LLVM_BOLT}, perf: ${PERF}")
    set(NY_BOLT_PROFILE "${CMAKE_BINARY_DIR}/bolt-profile.data" CACHE STRING "Path to perf.data for BOLT optimization")
    set(NY_BOLT_OPT_LEVEL "2" CACHE STRING "BOLT optimization level: 0|1|2|3")
    set(NY_BOLT_WORKLOAD "etc/tests/fuzz/bench" CACHE STRING "Path to benchmark workload for profiling")

    # Step 1: Instrument binary for profiling
    add_custom_target(bolt_instrument
      COMMAND ${CMAKE_COMMAND} -E echo "Instrumenting ny binary for BOLT profiling..."
      COMMAND ${LLVM_BOLT}
        $<TARGET_FILE:ny>
        -o $<TARGET_FILE:ny>.bolt-instr
        -instrument
        COMMENT "BOLT: Instrumenting ny binary"
      VERBATIM
    )
    add_dependencies(bolt_instrument ny)

    # Step 2: Run profiling with instrumented binary
    add_custom_target(bolt_profile
      COMMAND ${CMAKE_COMMAND} -E echo "Running BOLT profile collection..."
      COMMAND ${PERF} record -e branches:u -j any,u -o "${NY_BOLT_PROFILE}"
        -- $<TARGET_FILE:ny>.bolt-instr -c "
          use std.math.nt *
          mut i = 0
          while(i < 10000){
             def _ = power_mod(2, i, 1000000007)
             i += 1
          }
        "
      COMMENT "BOLT: Collecting profile data"
      VERBATIM
    )
    add_dependencies(bolt_profile bolt_instrument)

    # Step 3: Optimize binary with collected profile
    add_custom_target(bolt_ny
      COMMAND ${CMAKE_COMMAND} -E echo "Running BOLT optimization..."
      COMMAND ${LLVM_BOLT}
        $<TARGET_FILE:ny>
        -o $<TARGET_FILE:ny>.bolt
        -data "${NY_BOLT_PROFILE}"
        -reorder-blocks=cache+
        -reorder-functions=hfsort+
        -split-functions=3
        -split-all-cold
        -dyno-stats
        -icf=1
        -use-gnu-stack
        -peephole=1
        -lite=0
        COMMENT "BOLT: Optimizing ny binary layout"
      VERBATIM
    )
    add_dependencies(bolt_ny ny)
  else()
    if (LLVM_BOLT)
      message(WARNING "BOLT enabled but perf not found. Install linux-tools-common.")
    else()
      message(WARNING "BOLT enabled but llvm-bolt not found. Install LLVM tools.")
    endif()
  endif()
endif()
