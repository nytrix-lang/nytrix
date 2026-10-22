cmake_minimum_required(VERSION 3.16)

# CTest runs this file in script mode for the cross-binary CLOC comparison.
# Keeping the harness here avoids a second per-test CMake file.
function(_nytrix_run_cloc_parity_test)
  if (NOT DEFINED FMT OR NOT DEFINED NY OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "FMT, NY, and TEST_DIR are required")
  endif()

  file(REMOVE_RECURSE "${TEST_DIR}")
  set(long_dir
    "${TEST_DIR}/cloc_component_0123456789abcdef/cloc_component_0123456789abcdef/cloc_component_0123456789abcdef")
  file(MAKE_DIRECTORY "${long_dir}")
  set(source "${long_dir}/counted.ny")
  file(WRITE "${source}"
    ";; comment only\n"
    "fn answer() int {\n"
    "   /* block comment\n"
    "      continued */\n"
    "   return 42\n"
    "}\n")

  set(missing "${TEST_DIR}/missing")
  execute_process(
    COMMAND "${FMT}" --cloc "${missing}"
    RESULT_VARIABLE missing_rc
    OUTPUT_VARIABLE missing_output
    ERROR_VARIABLE missing_error
    TIMEOUT 20
  )
  execute_process(
    COMMAND "${NY}" fmt --cloc "${missing}"
    RESULT_VARIABLE missing_ny_rc
    OUTPUT_VARIABLE missing_ny_output
    ERROR_VARIABLE missing_ny_error
    TIMEOUT 20
  )
  if (NOT missing_rc EQUAL 1 OR NOT missing_ny_rc EQUAL 1 OR
      NOT missing_output STREQUAL missing_ny_output OR
      NOT missing_error STREQUAL missing_ny_error)
    message(FATAL_ERROR
      "embedded Ny CLOC invalid-root behavior differs from C oracle\n"
      "C rc=${missing_rc}, stdout=${missing_output}, stderr=${missing_error}\n"
      "Ny rc=${missing_ny_rc}, stdout=${missing_ny_output}, "
      "stderr=${missing_ny_error}")
  endif()

  execute_process(
    COMMAND "${FMT}" --cloc --json "${source}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 20
  )
  if (NOT rc EQUAL 0)
    message(FATAL_ERROR "ny fmt --cloc failed (${rc}): ${error}")
  endif()
  execute_process(
    COMMAND "${NY}" fmt --cloc --json "${source}"
    RESULT_VARIABLE ny_rc
    OUTPUT_VARIABLE ny_output
    ERROR_VARIABLE ny_error
    TIMEOUT 20
  )
  if (NOT ny_rc EQUAL 0)
    message(FATAL_ERROR "embedded ny fmt --cloc failed (${ny_rc}): ${ny_error}")
  endif()
  if (NOT output STREQUAL ny_output)
    message(FATAL_ERROR
      "embedded Ny CLOC JSON differs from standalone C oracle\n"
      "C oracle:\n${output}\n"
      "embedded Ny:\n${ny_output}")
  endif()

  execute_process(
    COMMAND "${FMT}" --cloc "${source}"
    RESULT_VARIABLE text_rc
    OUTPUT_VARIABLE text_output
    ERROR_VARIABLE text_error
    TIMEOUT 20
  )
  if (NOT text_rc EQUAL 0)
    message(FATAL_ERROR "C CLOC text oracle failed (${text_rc}): ${text_error}")
  endif()
  execute_process(
    COMMAND "${NY}" fmt --cloc "${source}"
    RESULT_VARIABLE text_ny_rc
    OUTPUT_VARIABLE text_ny_output
    ERROR_VARIABLE text_ny_error
    TIMEOUT 20
  )
  if (NOT text_ny_rc EQUAL 0)
    message(FATAL_ERROR
      "embedded Ny CLOC text route failed (${text_ny_rc}): ${text_ny_error}")
  endif()
  if (NOT text_output STREQUAL text_ny_output)
    message(FATAL_ERROR
      "embedded Ny CLOC text differs from standalone C oracle\n"
      "C oracle:\n${text_output}\n"
      "embedded Ny:\n${text_ny_output}")
  endif()

  execute_process(
    COMMAND "${FMT}" --cloc --json --full --top 1 "${source}"
    RESULT_VARIABLE full_rc
    OUTPUT_VARIABLE full_output
    ERROR_VARIABLE full_error
    TIMEOUT 20
  )
  if (NOT full_rc EQUAL 0)
    message(FATAL_ERROR "C CLOC full-row oracle failed (${full_rc}): ${full_error}")
  endif()
  execute_process(
    COMMAND "${NY}" fmt --cloc --json --full --top 1 "${source}"
    RESULT_VARIABLE full_ny_rc
    OUTPUT_VARIABLE full_ny_output
    ERROR_VARIABLE full_ny_error
    TIMEOUT 20
  )
  if (NOT full_ny_rc EQUAL 0)
    message(FATAL_ERROR
      "embedded Ny CLOC full-row route failed (${full_ny_rc}): ${full_ny_error}")
  endif()
  if (NOT full_output STREQUAL full_ny_output)
    message(FATAL_ERROR
      "embedded Ny CLOC full-row JSON differs from C oracle\n"
      "C oracle:\n${full_output}\n"
      "embedded Ny:\n${full_ny_output}")
  endif()

  execute_process(
    COMMAND "${FMT}" --cloc --full --top 1 "${source}"
    RESULT_VARIABLE full_text_rc
    OUTPUT_VARIABLE full_text_output
    ERROR_VARIABLE full_text_error
    TIMEOUT 20
  )
  if (NOT full_text_rc EQUAL 0)
    message(FATAL_ERROR
      "C CLOC full-row text oracle failed (${full_text_rc}): ${full_text_error}")
  endif()
  execute_process(
    COMMAND "${NY}" fmt --cloc --full --top 1 "${source}"
    RESULT_VARIABLE full_text_ny_rc
    OUTPUT_VARIABLE full_text_ny_output
    ERROR_VARIABLE full_text_ny_error
    TIMEOUT 20
  )
  if (NOT full_text_ny_rc EQUAL 0)
    message(FATAL_ERROR
      "embedded Ny CLOC full-row text route failed (${full_text_ny_rc}): "
      "${full_text_ny_error}")
  endif()
  if (NOT full_text_output STREQUAL full_text_ny_output)
    message(FATAL_ERROR
      "embedded Ny full-row CLOC text differs from standalone C oracle\n"
      "C oracle:\n${full_text_output}\n"
      "embedded Ny:\n${full_text_ny_output}")
  endif()

  string(JSON files GET "${output}" files)
  string(JSON loc_total GET "${output}" loc_total)
  string(JSON ny_files GET "${output}" ext ny files)
  string(JSON ny_loc GET "${output}" ext ny loc)
  if (NOT files EQUAL 1 OR NOT loc_total EQUAL 3 OR
      NOT ny_files EQUAL 1 OR NOT ny_loc EQUAL 3)
    message(FATAL_ERROR "unexpected cloc contract: ${output}")
  endif()
  string(JSON row_count LENGTH "${full_output}" rows)
  if (NOT row_count EQUAL 1)
    message(FATAL_ERROR "full-row CLOC contract failed: ${full_output}")
  endif()
endfunction()

if (NYTRIX_RUN_CLOC_PARITY)
  _nytrix_run_cloc_parity_test()
  return()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

project(nytrix C CXX)
include(GNUInstallDirs)
include(CTest)
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(FATAL_ERROR "In-source builds are not allowed. Create a separate build directory (e.g., build/native) and run cmake from there.")
endif()
if (APPLE)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
include(CheckLinkerFlag)
include(CheckCSourceCompiles)

set(NYTRIX_ROOT "${CMAKE_CURRENT_LIST_DIR}")

add_compile_definitions(_GNU_SOURCE)

option(NYTRIX_USE_LLVM "Enable LLVM backend and tooling" ON)
if (DEFINED ENV{NYTRIX_USE_LLVM})
  if ("$ENV{NYTRIX_USE_LLVM}" MATCHES "^(0|OFF|NO|FALSE|off|no|false)$")
    set(NYTRIX_USE_LLVM OFF CACHE BOOL "Enable LLVM backend and tooling" FORCE)
  else()
    set(NYTRIX_USE_LLVM ON CACHE BOOL "Enable LLVM backend and tooling" FORCE)
  endif()
endif()
option(NYTRIX_BUILD_SHARED "Build shared runtime library" ON)
option(NYTRIX_BUILD_LSP "Build ny-lsp" ON)
option(NYTRIX_STRICT_WARNINGS "Enable strict warning policy (pedantic + shadow + truncation diagnostics)" ON)
option(NYTRIX_RELEASE_DEBUG_INFO "Emit DWARF/CodeView info for Release builds" OFF)
option(NYTRIX_USE_ASAN "Enable AddressSanitizer and UBSanitizer" OFF)
option(NYTRIX_USE_GPROF "Enable gprof profiling (-pg)" OFF)
option(NYTRIX_FAST_BUILD "Optimize for current hardware (-march=native)" OFF)
option(NYTRIX_RUNTIME_O3 "Build runtime hot paths with -O3 in Release builds" ON)
option(NYTRIX_TOOLS_O3 "Build native C tool frontends with -O3 in Release builds" ON)
option(NYTRIX_ENABLE_TEST_MODE "Enable runtime and comptime test mode" OFF)

# Default strict warnings to ON for debug builds
if (CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT DEFINED NYTRIX_STRICT_WARNINGS_OVERRIDE)
  set(NYTRIX_STRICT_WARNINGS ON CACHE BOOL "Enable strict warning policy" FORCE)
endif()
option(NYTRIX_DEBUG_FRIENDLY "Enable debugger-friendly compile and link flags" ON)
option(NYTRIX_SPLIT_DWARF "Emit split DWARF debug info where supported" OFF)
set(NYTRIX_DWARF_VERSION "5" CACHE STRING "DWARF version for Unix-like debug builds")
set_property(CACHE NYTRIX_DWARF_VERSION PROPERTY STRINGS 2 3 4 5)

set(NYTRIX_VERSION_MAJOR "0")
set(NYTRIX_VERSION_MINOR "10")
set(NYTRIX_VERSION_PATCH "0")
set(NYTRIX_VERSION_REF "" CACHE STRING
    "Git ref used for version metadata; empty prefers the upstream remote ref")
set(NYTRIX_VERSION_HEADER "${CMAKE_BINARY_DIR}/nytrix_version.h")
set(NYTRIX_VERSION_STAMP "${CMAKE_BINARY_DIR}/nytrix_version.stamp")
set(STD_ID_HEADER "${CMAKE_BINARY_DIR}/nytrix_std_id.h")
set(_NYTRIX_STD_ID_SCRIPT "${CMAKE_BINARY_DIR}/nytrix_std_id.cmake")
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
  # The true build id is HEAD plus dirtiness: a version string derived from
  # an upstream ref alone cannot tell a dev build (or a stale install) from
  # the release it was cut from. HEAD stays informational alongside COMMIT so
  # release numbering is unchanged.
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_root}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE _head_hash
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT _head_hash MATCHES "^[0-9a-f]+$")
    set(_head_hash "source")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_root}" status --porcelain
    OUTPUT_VARIABLE _porcelain
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(_porcelain STREQUAL "")
    set(_head_dirty 0)
  else()
    set(_head_dirty 1)
  endif()
endif()

if(NOT DEFINED _head_hash)
  set(_head_hash "source")
endif()
if(NOT DEFINED _head_dirty)
  set(_head_dirty 0)
endif()
if(_head_dirty)
  set(_dirty 1)
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
#define NYTRIX_VERSION_HEAD \"${_head_hash}\"
#define NYTRIX_VERSION_HEAD_DIRTY ${_head_dirty}
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
# The dirty flag must refresh on every build (worktree edits do not touch git
# metadata, so dependency tracking alone would keep reporting a clean tree).
# The generator script rewrites the header only when content actually changed,
# so steady-state builds pay script runs and no recompiles.
add_custom_target(nytrix_version_header ALL
  COMMAND ${CMAKE_COMMAND} -P "${NYTRIX_VERSION_GENERATOR}"
  COMMAND ${CMAKE_COMMAND}
    "-DNYTRIX_STD_LIBDIR=${NYTRIX_ROOT}/lib"
    "-DNYTRIX_STD_ID_OUT=${STD_ID_HEADER}" -P "${_NYTRIX_STD_ID_SCRIPT}"
  COMMENT "Refreshing Git-derived version header"
  VERBATIM
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
if (NYTRIX_USE_LLVM)
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
    elseif(APPLE)
      foreach(_brew_llvm_version 23 22 21 20 19 18)
        if(EXISTS "/opt/homebrew/opt/llvm@${_brew_llvm_version}/bin/llvm-config")
          set(LLVM_CONFIG_EXE "/opt/homebrew/opt/llvm@${_brew_llvm_version}/bin/llvm-config")
          break()
        elseif(EXISTS "/usr/local/opt/llvm@${_brew_llvm_version}/bin/llvm-config")
          set(LLVM_CONFIG_EXE "/usr/local/opt/llvm@${_brew_llvm_version}/bin/llvm-config")
          break()
        endif()
      endforeach()
      if(NOT LLVM_CONFIG_EXE AND EXISTS "/opt/homebrew/opt/llvm/bin/llvm-config")
        set(LLVM_CONFIG_EXE "/opt/homebrew/opt/llvm/bin/llvm-config")
      elseif(NOT LLVM_CONFIG_EXE AND EXISTS "/usr/local/opt/llvm/bin/llvm-config")
        set(LLVM_CONFIG_EXE "/usr/local/opt/llvm/bin/llvm-config")
      endif()
    else()
      find_program(LLVM_CONFIG_EXE
                   NAMES llvm-config-23 llvm-config-22 llvm-config-21 llvm-config-20 llvm-config-19
                         llvm-config-18 llvm-config-17 llvm-config-16 llvm-config
                   PATHS "$ENV{MSYSTEM_PREFIX}/bin" "$ENV{MINGW_PREFIX}/bin"
                         "C:/msys64/ucrt64/bin" "C:/msys64/clang64/bin"
                         "C:/msys64/mingw64/bin" "C:/msys64/clangarm64/bin"
                         "C:/tools/msys64/ucrt64/bin"
                         "C:/tools/msys64/clang64/bin" "C:/tools/msys64/mingw64/bin"
                         "C:/tools/msys64/clangarm64/bin"
                         "C:/Program Files/LLVM/bin"
                         "C:/tools/llvm/bin" "/opt/homebrew/opt/llvm@23/bin"
                         "/usr/local/opt/llvm@23/bin" "/opt/homebrew/opt/llvm@22/bin"
                         "/usr/local/opt/llvm@22/bin" "/opt/homebrew/opt/llvm@20/bin"
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
            NYTRIX_LLVM_VERSION_MAJOR VERSION_GREATER 23)
          message(FATAL_ERROR
            "Unsupported LLVM ${LLVM_VERSION_RAW}. Nytrix supports LLVM 16 through 23; "
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
      endif()
    endif()

    set(NYTRIX_CLANG_INCLUDE "" CACHE PATH "Clang include root")
    if (NOT NYTRIX_CLANG_INCLUDE)
      if (NYTRIX_LLVM_INCLUDE AND EXISTS "${NYTRIX_LLVM_INCLUDE}/clang-c/Index.h")
        set(NYTRIX_CLANG_INCLUDE "${NYTRIX_LLVM_INCLUDE}")
      elseif(EXISTS "/usr/include/clang-c/Index.h")
        set(NYTRIX_CLANG_INCLUDE "/usr/include")
      elseif(EXISTS "/usr/local/include/clang-c/Index.h")
        set(NYTRIX_CLANG_INCLUDE "/usr/local/include")
      elseif(EXISTS "/opt/homebrew/include/clang-c/Index.h")
        set(NYTRIX_CLANG_INCLUDE "/opt/homebrew/include")
      else()
        find_path(NYTRIX_CLANG_INCLUDE
          NAMES clang-c/Index.h
          PATHS
            "${NYTRIX_LLVM_INCLUDE}"
            "/usr/include"
            "/usr/local/include"
            "/opt/homebrew/include"
            "/usr/lib/llvm-${NYTRIX_LLVM_VERSION_MAJOR}/include"
            "/usr/lib/llvm${NYTRIX_LLVM_VERSION_MAJOR}/include"
            "C:/Program Files/LLVM/include"
            "C:/msys64/ucrt64/include"
            "C:/msys64/clang64/include"
            "C:/msys64/mingw64/include"
            "$ENV{MSYSTEM_PREFIX}/include"
            "$ENV{MINGW_PREFIX}/include"
        )
      endif()
    endif()

    if (WIN32 AND (NOT NYTRIX_LLVM_INCLUDE OR NOT NYTRIX_CLANG_INCLUDE))
      message(FATAL_ERROR "LLVM/Clang include directory not found! Verified standard paths and ENV{NYTRIX_LLVM_INCLUDE}. Please install LLVM+libclang or set NYTRIX_LLVM_INCLUDE to the include directory containing llvm-c/Core.h and clang-c/Index.h.")
    endif()
  endif()

  if (NOT NYTRIX_LLVM_INCLUDE OR NOT NYTRIX_CLANG_INCLUDE)
    if (WIN32)
      message(FATAL_ERROR "LLVM/Clang development headers are required (missing llvm-c/Core.h or clang-c/Index.h). Install llvm-dev and libclang-dev, or set NYTRIX_LLVM_INCLUDE.")
    else()
      message(WARNING "LLVM/Clang development headers not found (missing llvm-c/Core.h or clang-c/Index.h). Disabling LLVM support.")
      set(NYTRIX_USE_LLVM OFF CACHE BOOL "Enable LLVM backend and tooling" FORCE)
    endif()
  endif()
endif()

if (NYTRIX_USE_LLVM)
  message(STATUS "Final NYTRIX_LLVM_INCLUDE: '${NYTRIX_LLVM_INCLUDE}'")
  message(STATUS "Final NYTRIX_CLANG_INCLUDE: '${NYTRIX_CLANG_INCLUDE}'")

  nytrix_parse_flag_list(NYTRIX_LLVM_CFLAGS_LIST "${NYTRIX_LLVM_CFLAGS}")
  # For LDFLAGS, we need to be careful not to split paths incorrectly, but separate_arguments usually handles NATIVE_COMMAND well.
  nytrix_parse_flag_list(NYTRIX_LLVM_LDFLAGS_LIST "${NYTRIX_LLVM_LDFLAGS}")

  if (NYTRIX_LLVM_INCLUDE)
    file(TO_CMAKE_PATH "${NYTRIX_LLVM_INCLUDE}" NYTRIX_LLVM_INCLUDE)
    string(STRIP "${NYTRIX_LLVM_INCLUDE}" NYTRIX_LLVM_INCLUDE)
    set(NYTRIX_LLVM_INCLUDE "${NYTRIX_LLVM_INCLUDE}" CACHE STRING "LLVM include root" FORCE)
    include_directories(SYSTEM "${NYTRIX_LLVM_INCLUDE}")
  endif()
  if (NYTRIX_CLANG_INCLUDE)
    file(TO_CMAKE_PATH "${NYTRIX_CLANG_INCLUDE}" NYTRIX_CLANG_INCLUDE)
    string(STRIP "${NYTRIX_CLANG_INCLUDE}" NYTRIX_CLANG_INCLUDE)
    set(NYTRIX_CLANG_INCLUDE "${NYTRIX_CLANG_INCLUDE}" CACHE STRING "Clang include root" FORCE)
    include_directories(SYSTEM "${NYTRIX_CLANG_INCLUDE}")
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
    "/opt/homebrew/opt/llvm@22/lib"
    "/usr/local/opt/llvm@22/lib"
    "/opt/homebrew/opt/llvm@21/lib"
    "/usr/local/opt/llvm@21/lib"
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
    "/usr/lib/x86_64-linux-gnu"
    "/usr/lib/aarch64-linux-gnu"
    "/usr/lib"
    "/usr/local/lib"
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
    set(_nytrix_clang_names
      libclang
      clang
      libclang.so.1
      libclang.so
      libclang.dll.a
      clang.dll.a
    )
    if (NYTRIX_LLVM_VERSION_MAJOR)
      list(PREPEND _nytrix_clang_names
        "clang-${NYTRIX_LLVM_VERSION_MAJOR}"
        "libclang-${NYTRIX_LLVM_VERSION_MAJOR}"
        "libclang-${NYTRIX_LLVM_VERSION_MAJOR}.so.1"
        "libclang-${NYTRIX_LLVM_VERSION_MAJOR}.so"
        "libclang.so.${NYTRIX_LLVM_VERSION_MAJOR}"
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
endif()
set(NYTRIX_WINSDK_CFLAGS "" CACHE STRING "Windows SDK include flags list")
set(NYTRIX_WINSDK_LDFLAGS "" CACHE STRING "Windows SDK lib flags list")

set(NYTRIX_HOST_CFLAGS "" CACHE STRING "Host-specific C flags (semicolon-separated)")
set(NYTRIX_HOST_LDFLAGS "" CACHE STRING "Host-specific linker flags (semicolon-separated)")
set(NYTRIX_LINKER "system" CACHE STRING "Build linker: system|lld|mold")
set_property(CACHE NYTRIX_LINKER PROPERTY STRINGS system lld mold)
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

option(NYTRIX_ENABLE_OPENMP "Enable OpenMP compiler parallelism" ON)
if (NYTRIX_ENABLE_OPENMP)
  find_package(OpenMP QUIET COMPONENTS C)
  if (OpenMP_C_FOUND)
    message(STATUS "OpenMP compiler parallelism enabled")
    link_libraries(OpenMP::OpenMP_C)
  else()
    message(STATUS "OpenMP not found; using C11 thread fallback where available")
  endif()
endif()

set(NYTRIX_ENABLE_Z3 "auto" CACHE STRING "Enable Z3-backed finite constraint solving: auto|on|off")
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
      "${NYTRIX_ROOT}/build/vendor/include"
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
  elseif (NYTRIX_Z3_LIBRARY AND NOT NYTRIX_Z3_INCLUDE_DIR)
    message(STATUS "Z3 library found at ${NYTRIX_Z3_LIBRARY}, but development header z3.h is missing; install Z3 development headers or place z3*.h in build/vendor/include; using HM-only type solver")
  elseif (NYTRIX_Z3_INCLUDE_DIR AND NOT NYTRIX_Z3_LIBRARY)
    message(STATUS "Z3 headers found at ${NYTRIX_Z3_INCLUDE_DIR}, but libz3 is missing; install the matching Z3 library or place it in build/vendor/lib/host; using HM-only type solver")
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

set(CORE_NY "${CMAKE_BINARY_DIR}/core.ny")
set(CORE_SYMBOLS "${CMAKE_BINARY_DIR}/core_symbols.h")
# Records the exact stdlib sources a binary was built against so the launcher
# can warn/refuse on skewed binary+stdlib pairs. Hashes sources (not the
# bundle) so it needs no built binary and creates no build cycle; runs with
# the always-on version target and rewrites only on change.
file(WRITE "${_NYTRIX_STD_ID_SCRIPT}"
  "if(NOT DEFINED NYTRIX_STD_LIBDIR OR NOT DEFINED NYTRIX_STD_ID_OUT)\n"
  "  message(FATAL_ERROR \"NYTRIX_STD_LIBDIR and NYTRIX_STD_ID_OUT required\")\n"
  "endif()\n"
  "file(GLOB_RECURSE _std_files LIST_DIRECTORIES false \"\${NYTRIX_STD_LIBDIR}/*.ny\")\n"
  "list(SORT _std_files)\n"
  "set(_combined \"\")\n"
  "foreach(_f IN LISTS _std_files)\n"
  "  file(SHA256 \"\${_f}\" _h)\n"
  "  string(APPEND _combined \"\${_h}\")\n"
  "endforeach()\n"
  "string(SHA256 _std_sha \"\${_combined}\")\n"
  "list(LENGTH _std_files _std_count)\n"
  "set(_id_contents \"#pragma once\n\n#define NYTRIX_STD_SHA256 \\\"\${_std_sha}\\\"\n#define NYTRIX_STD_FILES \${_std_count}\n\")\n"
  "if(EXISTS \"\${NYTRIX_STD_ID_OUT}\")\n"
  "  file(READ \"\${NYTRIX_STD_ID_OUT}\" _id_old)\n"
  "else()\n"
  "  set(_id_old \"\")\n"
  "endif()\n"
  "if(NOT _id_old STREQUAL _id_contents)\n"
  "  file(WRITE \"\${NYTRIX_STD_ID_OUT}\" \"\${_id_contents}\")\n"
  "endif()\n")
file(GLOB_RECURSE STD_SOURCES_LIB CONFIGURE_DEPENDS "${NYTRIX_ROOT}/lib/*.ny")
set(STD_SOURCES ${STD_SOURCES_LIB})

option(NYTRIX_STD_VERBOSE "Verbose core.ny generation output" OFF)
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
if (NYTRIX_USE_LLVM)
  add_custom_command(
    OUTPUT "${CORE_NY}"
    BYPRODUCTS "${CORE_SYMBOLS}"
    COMMAND ${_NYTRIX_STD_ENV} $<TARGET_FILE:ny> --bundle-core="${CORE_NY}" --bundle-symbols="${CORE_SYMBOLS}" --verbose=${_NYTRIX_STD_VERBOSE_LEVEL}
    WORKING_DIRECTORY "${NYTRIX_ROOT}"
    DEPENDS ny ${STD_SOURCES}
    COMMENT "Generating core.ny"
  )

  set(CORE_BC "${CMAKE_BINARY_DIR}/core.bc")
  set(_NYTRIX_CORE_BC_SIGNATURE
    "${CMAKE_BINARY_DIR}/core.bc.signature")
  set(_NYTRIX_CORE_BC_INPUTS
    "${CMAKE_BINARY_DIR}/core.bc.inputs")
  set(_NYTRIX_CORE_BC_CONFIG
    "${CMAKE_C_COMPILER_ID}|${CMAKE_C_COMPILER_VERSION}|${CMAKE_BUILD_TYPE}|${CMAKE_C_FLAGS}|${CMAKE_C_FLAGS_RELEASE}|${CMAKE_EXE_LINKER_FLAGS}|${NYTRIX_USE_LLVM}|${LLVM_VERSION_RAW}|${NYTRIX_LLVM_LIBDIR}|${NYTRIX_CLANG_LIBRARY_FOUND}|${NYTRIX_FAST_BUILD}|${NYTRIX_RUNTIME_O3}|${NYTRIX_LTO_MODE}")
  string(SHA256 _NYTRIX_CORE_BC_CONFIG_HASH "${_NYTRIX_CORE_BC_CONFIG}")
  # Keep the shared core bitcode cache builder self-contained in this file.
  # Write its build-time script into the binary tree so concurrent builds can
  # serialize through file(LOCK) without adding a repository helper directory.
  set(_NYTRIX_CORE_BC_BUILDER
    "${CMAKE_BINARY_DIR}/build_core_bc.cmake")
  file(WRITE "${_NYTRIX_CORE_BC_BUILDER}" [=[
if(NOT DEFINED NYTRIX_CORE_BC OR NOT DEFINED NYTRIX_CORE_NY OR
   NOT DEFINED NYTRIX_NY_BIN OR NOT DEFINED NYTRIX_ROOT OR
   NOT DEFINED NYTRIX_CORE_SIGNATURE OR NOT DEFINED NYTRIX_CORE_INPUTS OR
   NOT DEFINED NYTRIX_C_COMPILER OR
   NOT DEFINED NYTRIX_CORE_BC_CONFIG_HASH)
  message(FATAL_ERROR "missing core bitcode builder arguments")
endif()

file(LOCK "${NYTRIX_CORE_BC}.lock" GUARD PROCESS TIMEOUT 1800
  RESULT_VARIABLE _lock_result)
if(NOT _lock_result STREQUAL "0")
  message(FATAL_ERROR "could not lock core bitcode cache: ${_lock_result}")
endif()

if(NOT EXISTS "${NYTRIX_CORE_NY}")
  message(FATAL_ERROR "missing core library cache inputs")
endif()
file(SHA256 "${NYTRIX_CORE_NY}" _std_hash)
set(_old_signature_content "")
set(_old_input_identity "")
if(EXISTS "${NYTRIX_CORE_SIGNATURE}")
  file(READ "${NYTRIX_CORE_SIGNATURE}" _old_signature_content)
endif()
if(EXISTS "${NYTRIX_CORE_INPUTS}")
  file(READ "${NYTRIX_CORE_INPUTS}" _old_input_identity)
endif()
set(_compiler_path "${NYTRIX_NY_BIN}")
if(DEFINED ENV{NYTRIX_CORE_BC_COMPILER} AND
   EXISTS "$ENV{NYTRIX_CORE_BC_COMPILER}")
  # Test suites pin one ny-full snapshot while concurrent builds may replace
  # the regular target. Build and fingerprint the shared artifact with the
  # same executable the suite will use.
  set(_compiler_path "$ENV{NYTRIX_CORE_BC_COMPILER}")
endif()
file(SHA256 "${_compiler_path}" _compiler_binary_hash)
set(_compiler_sources "${NYTRIX_ROOT}/CMakeLists.txt")
foreach(_source_dir IN ITEMS src/base src/code src/cmd/ny src/include src/sema)
  file(GLOB_RECURSE _source_files LIST_DIRECTORIES false
    "${NYTRIX_ROOT}/${_source_dir}/*")
  list(APPEND _compiler_sources ${_source_files})
endforeach()
list(APPEND _compiler_sources "${NYTRIX_C_COMPILER}"
  "${CMAKE_BINARY_DIR}/CMakeCache.txt")
list(REMOVE_DUPLICATES _compiler_sources)
list(SORT _compiler_sources)
set(_compiler_identity_input "${NYTRIX_CORE_BC_CONFIG_HASH}\n")
foreach(_source IN LISTS _compiler_sources)
  if(EXISTS "${_source}" AND NOT IS_DIRECTORY "${_source}")
    file(SHA256 "${_source}" _source_hash)
    file(RELATIVE_PATH _source_name "${NYTRIX_ROOT}" "${_source}")
    string(APPEND _compiler_identity_input "${_source_name}\t${_source_hash}\n")
  endif()
endforeach()
string(SHA256 _compiler_source_hash "${_compiler_identity_input}")
set(_legacy_signature
  "artifact=core.bc\nselector=lib\nformat=nyir-bodies-v2\nentry=no\n")
set(_signature_content
  "${_legacy_signature}compiler-source=${_compiler_source_hash}\n")
set(_signature_pending "${NYTRIX_CORE_SIGNATURE}.pending")
file(WRITE "${_signature_pending}" "${_signature_content}")
file(RENAME "${_signature_pending}" "${NYTRIX_CORE_SIGNATURE}")
file(SHA256 "${NYTRIX_CORE_SIGNATURE}" _signature_hash)
set(_input_identity "${_std_hash}\n${_signature_hash}\n")
set(_legacy_signature_hash "")
string(SHA256 _legacy_signature_hash "${_old_signature_content}")
set(_legacy_input_identity
  "${_std_hash}\n${_legacy_signature_hash}\n${_compiler_binary_hash}\n")
if(EXISTS "${NYTRIX_CORE_BC}" AND
   _old_signature_content STREQUAL _legacy_signature AND
   _old_input_identity STREQUAL _legacy_input_identity)
  file(SIZE "${NYTRIX_CORE_BC}" _legacy_artifact_size)
  if(_legacy_artifact_size GREATER 0)
    # Safely migrate a prior cache only when its exact compiler binary,
    # standard source, and legacy signature match.
    file(TOUCH "${NYTRIX_CORE_BC}")
    file(WRITE "${NYTRIX_CORE_INPUTS}.pending" "${_input_identity}")
    file(RENAME "${NYTRIX_CORE_INPUTS}.pending" "${NYTRIX_CORE_INPUTS}")
    message(STATUS "Reusing core.bc (verified legacy build identity)")
    return()
  endif()
endif()
if(EXISTS "${NYTRIX_CORE_BC}" AND
   _old_input_identity STREQUAL _input_identity)
  file(SIZE "${NYTRIX_CORE_BC}" _cached_artifact_size)
  if(_cached_artifact_size GREATER 0)
    # The compiler may have regenerated identical core.ny. Its
    # timestamp is not an input identity, so preserve the expensive bitcode
    # artifact and simply refresh its freshness for the build graph.
    file(TOUCH "${NYTRIX_CORE_BC}")
    message(STATUS "Reusing core.bc (core and compiler hashes unchanged)")
    return()
  endif()
endif()

# Consumers lazily materialize this bitcode from a memory mapping. Never
# truncate the published file while a fixture/compiler is still using it.
set(_pending_artifact "${NYTRIX_CORE_BC}.pending")
file(REMOVE "${_pending_artifact}" "${_pending_artifact}.links")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    NYTRIX_STD_THREADS=16
    NYTRIX_STD_BC_EMIT_BODIES=1
    "${_compiler_path}"
    -std full
    "--core-path=${NYTRIX_CORE_NY}"
    -emit-only
    "--emit-bc=${_pending_artifact}"
    --emit-module=lib
    -e " "
  WORKING_DIRECTORY "${NYTRIX_ROOT}"
  RESULT_VARIABLE _compile_result)
if(NOT _compile_result EQUAL 0)
  message(FATAL_ERROR "failed to build shared core bitcode cache")
endif()
if(NOT EXISTS "${_pending_artifact}")
  message(FATAL_ERROR "compiler did not create ${NYTRIX_CORE_BC}")
endif()
file(SIZE "${_pending_artifact}" _artifact_size)
if(_artifact_size EQUAL 0)
  message(FATAL_ERROR "compiler created an empty ${NYTRIX_CORE_BC}")
endif()
file(RENAME "${_pending_artifact}" "${NYTRIX_CORE_BC}")
if(EXISTS "${_pending_artifact}.links")
  file(RENAME "${_pending_artifact}.links" "${NYTRIX_CORE_BC}.links")
endif()
file(WRITE "${NYTRIX_CORE_INPUTS}.pending" "${_input_identity}")
file(RENAME "${NYTRIX_CORE_INPUTS}.pending" "${NYTRIX_CORE_INPUTS}")
]=])
  set(_NYTRIX_VERIFY_NONEMPTY "${CMAKE_BINARY_DIR}/verify_nonempty_artifact.cmake")
  file(WRITE "${_NYTRIX_VERIFY_NONEMPTY}"
    "if(NOT EXISTS \"\${NYTRIX_ARTIFACT}\")\n"
    "  message(FATAL_ERROR \"expected artifact was not created: \${NYTRIX_ARTIFACT}\")\n"
    "endif()\n"
    "file(SIZE \"\${NYTRIX_ARTIFACT}\" _artifact_size)\n"
    "if(_artifact_size EQUAL 0)\n"
    "  message(FATAL_ERROR \"expected nonempty artifact: \${NYTRIX_ARTIFACT}\")\n"
    "endif()\n")
  add_custom_target(core_bc
    COMMAND "${CMAKE_COMMAND}"
      "-DNYTRIX_CORE_BC=${CORE_BC}"
      "-DNYTRIX_CORE_NY=${CORE_NY}"
      "-DNYTRIX_NY_BIN=$<TARGET_FILE:ny-full>"
      "-DNYTRIX_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DNYTRIX_CORE_BC_CONFIG_HASH=${_NYTRIX_CORE_BC_CONFIG_HASH}"
      "-DNYTRIX_ROOT=${NYTRIX_ROOT}"
      "-DNYTRIX_CORE_SIGNATURE=${_NYTRIX_CORE_BC_SIGNATURE}"
      "-DNYTRIX_CORE_INPUTS=${_NYTRIX_CORE_BC_INPUTS}"
      -P "${_NYTRIX_CORE_BC_BUILDER}"
    COMMAND "${CMAKE_COMMAND}" "-DNYTRIX_ARTIFACT=${CORE_BC}" -P "${_NYTRIX_VERIFY_NONEMPTY}"
    DEPENDS "${CORE_NY}" "${_NYTRIX_CORE_BC_BUILDER}" ny-full
    COMMENT "Pre-compiling core library bitcode"
    VERBATIM
  )
  add_custom_target(core ALL DEPENDS "${CORE_NY}" "${CORE_SYMBOLS}")
  add_custom_target(std DEPENDS core)
  # core.bc generation holds the whole core library implementation in one LLVM
  # module and can grow past tens of GiB RSS, and a killed run must never block the default
  # `all` build. Keep it out of `all`; the full `make test` path explicitly
  # builds this shared artifact once before launching fixture workers.
  add_custom_target(std_bc DEPENDS core_bc)
else()
  add_custom_command(
    OUTPUT "${CORE_NY}" "${CORE_SYMBOLS}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CORE_NY}" "${CORE_SYMBOLS}"
    COMMENT "Creating dummy core.ny"
  )
  add_custom_target(core ALL DEPENDS "${CORE_NY}" "${CORE_SYMBOLS}")
  add_custom_target(std DEPENDS core)
  add_custom_target(core_bc)
  add_custom_target(std_bc DEPENDS core_bc)
endif()

file(GLOB SRC_BASE CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/base/*.c")
file(GLOB SRC_SEMA CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/sema/*.c")
file(GLOB SRC_CODE CONFIGURE_DEPENDS
  "${NYTRIX_ROOT}/src/code/*.c"
  "${NYTRIX_ROOT}/src/code/lang/parse/frontend/*.c"
  "${NYTRIX_ROOT}/src/code/lang/c/c_lex.c"
  "${NYTRIX_ROOT}/src/code/lang/c/c_parse.c"
  "${NYTRIX_ROOT}/src/code/lang/c/clang.c"
  "${NYTRIX_ROOT}/src/code/lang/llvm/*.c"
  "${NYTRIX_ROOT}/src/code/lang/llvm/calls/*.c"
  "${NYTRIX_ROOT}/src/code/lang/llvm/comptime/*.c"
  "${NYTRIX_ROOT}/src/code/lang/llvm/expr/*.c"
  "${NYTRIX_ROOT}/src/code/backend/wire/incremental/*.c"
  "${NYTRIX_ROOT}/src/code/lang/types/*.c"
  "${NYTRIX_ROOT}/src/code/backend/llvm/*.c"
  "${NYTRIX_ROOT}/src/code/lang/*.c"
  "${NYTRIX_ROOT}/src/code/backend/*.c"
  "${NYTRIX_ROOT}/src/code/backend/ir/*.c"
  "${NYTRIX_ROOT}/src/code/backend/ir/opt/*.c"
  "${NYTRIX_ROOT}/src/code/backend/object/*.c"
  "${NYTRIX_ROOT}/src/code/backend/machine/*.c")
file(GLOB SRC_REPL CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/cmd/ny/repl/*.c")
file(GLOB SRC_WIRE CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/code/backend/wire/*.c")

# These source groups are amalgamated by their *_init.c owners.  Keep each
# sibling set globbed once so it drives both standalone exclusion and rebuild
# dependencies when a fragment is added or removed.
file(GLOB TYPING_PIPELINE_SOURCES CONFIGURE_DEPENDS
  "${NYTRIX_ROOT}/src/code/lang/types/pipeline/*.c")
file(GLOB LLVM_EXPR_SOURCES CONFIGURE_DEPENDS
  "${NYTRIX_ROOT}/src/code/lang/llvm/expr/*.c")
list(REMOVE_ITEM LLVM_EXPR_SOURCES
  "${NYTRIX_ROOT}/src/code/lang/llvm/expr/init.c")
list(REMOVE_ITEM SRC_CODE ${TYPING_PIPELINE_SOURCES})
list(REMOVE_ITEM SRC_CODE ${LLVM_EXPR_SOURCES})
set(SRC_COMPILER
  ${SRC_BASE}
  ${SRC_SEMA}
  ${SRC_CODE}
  ${SRC_REPL}
  ${SRC_WIRE}
  "${NYTRIX_ROOT}/src/code/lang/parse/ast.c"
  "${NYTRIX_ROOT}/src/code/lang/parse/dump.c"
  "${NYTRIX_ROOT}/src/code/lang/parse/lexer.c"
  "${NYTRIX_ROOT}/src/code/lang/parse/shared.c"
  "${NYTRIX_ROOT}/src/code/lang/types/pipeline/init.c"
  "${NYTRIX_ROOT}/src/code/lang/types/policy/check.c"
  "${NYTRIX_ROOT}/src/code/lang/pipeline/init.c"
  "${NYTRIX_ROOT}/src/code/lang/pipeline/build.c"
  "${NYTRIX_ROOT}/src/code/lang/pipeline/native.c"
  "${NYTRIX_ROOT}/src/code/lang/pipeline/native_vm.c"
  "${NYTRIX_ROOT}/src/code/lang/pipeline/emit.c"
  "${NYTRIX_ROOT}/src/code/lang/pipeline/x86_64_legacy.c"
  "${NYTRIX_ROOT}/src/code/lang/pipeline/embed.c"
  "${NYTRIX_ROOT}/src/code/backend/ir/public.c"
)
list(REMOVE_ITEM SRC_COMPILER
  "${NYTRIX_ROOT}/src/code/backend/abi.c")

# Several compiler subsystems are intentionally built as amalgamated translation
# units: their init.c file textually includes sibling .c files.  CMake only
# tracks the top-level source by default, so incremental builds could silently
# keep stale lowering/type/wire code after an included file changed.  Make the
# include relationship explicit just as we do for runtime/init.c below.
file(GLOB LLVM_CALLS_SOURCES CONFIGURE_DEPENDS
  "${NYTRIX_ROOT}/src/code/lang/llvm/calls/*.c")
# calls/init.c and expr/init.c textually include their sibling fragments;
# LLVM_CALLS_SOURCES keeps the fragment list (minus init.c) so the blocks
# below remove those units from the standalone compile.
list(REMOVE_ITEM LLVM_CALLS_SOURCES
  "${NYTRIX_ROOT}/src/code/lang/llvm/calls/init.c")
file(GLOB PARSE_AMALGAMATION_SOURCES CONFIGURE_DEPENDS
  "${NYTRIX_ROOT}/src/code/lang/parse/*.c"
  "${NYTRIX_ROOT}/src/code/lang/parse/stmt/*.c")
file(GLOB WIRE_PIPE_SOURCES CONFIGURE_DEPENDS
  "${NYTRIX_ROOT}/src/code/lang/pipeline/*.c")
list(REMOVE_ITEM SRC_COMPILER
  ${LLVM_CALLS_SOURCES}
  ${LLVM_EXPR_SOURCES})
list(REMOVE_ITEM PARSE_AMALGAMATION_SOURCES
  "${NYTRIX_ROOT}/src/code/lang/parse/ast.c"
  "${NYTRIX_ROOT}/src/code/lang/parse/dump.c"
  "${NYTRIX_ROOT}/src/code/lang/parse/lexer.c"
  "${NYTRIX_ROOT}/src/code/lang/parse/shared.c")
list(REMOVE_ITEM TYPING_PIPELINE_SOURCES
  "${NYTRIX_ROOT}/src/code/lang/types/pipeline/init.c")
list(REMOVE_ITEM WIRE_PIPE_SOURCES
  "${NYTRIX_ROOT}/src/code/lang/pipeline/init.c"
  "${NYTRIX_ROOT}/src/code/lang/pipeline/embed.c")
set_source_files_properties(
  "${NYTRIX_ROOT}/src/code/lang/llvm/calls/init.c"
  PROPERTIES OBJECT_DEPENDS "${LLVM_CALLS_SOURCES}")
set_source_files_properties(
  "${NYTRIX_ROOT}/src/code/lang/llvm/expr/init.c"
  PROPERTIES OBJECT_DEPENDS "${LLVM_EXPR_SOURCES}")
set_source_files_properties(
  "${NYTRIX_ROOT}/src/code/lang/parse/shared.c"
  PROPERTIES OBJECT_DEPENDS "${PARSE_AMALGAMATION_SOURCES}")
set_source_files_properties(
  "${NYTRIX_ROOT}/src/code/lang/types/pipeline/init.c"
  PROPERTIES OBJECT_DEPENDS "${TYPING_PIPELINE_SOURCES}")
set_source_files_properties(
  "${NYTRIX_ROOT}/src/code/lang/pipeline/init.c"
  PROPERTIES OBJECT_DEPENDS "${WIRE_PIPE_SOURCES}")

set(RT_INIT "${NYTRIX_ROOT}/src/code/lang/runtime/init.c")
file(GLOB RT_SOURCES CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/code/lang/runtime/*.c")
file(GLOB RT_HEADERS CONFIGURE_DEPENDS "${NYTRIX_ROOT}/src/code/lang/runtime/*.h")
list(REMOVE_ITEM RT_SOURCES "${RT_INIT}")

# Runtime is compiled via src/code/lang/runtime/init.c (which #includes the other runtime/*.c
# files). Mark all runtime sources/headers as dependencies so Ninja rebuilds
# ny/ny_debug when any runtime unit changes.
set_source_files_properties("${RT_INIT}" PROPERTIES
  OBJECT_DEPENDS "${RT_SOURCES};${RT_HEADERS}"
)
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
    endif()
endif()

if (MSVC)
  set(NYTRIX_WARNING_FLAGS /W4)
elseif (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  set(NYTRIX_WARNING_FLAGS
    -Wall
    -Wextra
    -Wunused
    -Wmissing-prototypes
    -Wno-implicit-fallthrough
    -Wstrict-prototypes
    -Wundef
    -Wwrite-strings
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
  if (CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(NYTRIX_CLANG_WARNING_FLAGS
      -Wno-gnu-include-next
      -Wno-gnu-zero-variadic-macro-arguments
      -Wno-language-extension-token
      -Wno-unneeded-internal-declaration
    )
  endif()
endif()

# Optimization and frame pointers
if (MSVC)
  add_compile_options($<$<CONFIG:Debug>:/Od> $<$<CONFIG:Release>:/O2>)
else()
  add_compile_options($<$<CONFIG:Debug>:-O0> $<$<CONFIG:Release>:-O2>)
  add_compile_options(-fno-omit-frame-pointer)
  if (NOT APPLE AND NOT NYTRIX_LINKER STREQUAL "system")
    if (NYTRIX_LINKER STREQUAL "mold")
      find_program(NYTRIX_SELECTED_LINKER NAMES mold REQUIRED)
    elseif (NYTRIX_LINKER STREQUAL "lld")
      find_program(NYTRIX_SELECTED_LINKER NAMES ld.lld lld REQUIRED)
    else()
      message(FATAL_ERROR "NYTRIX_LINKER must be system, lld, or mold")
    endif()
    message(STATUS "Using requested ${NYTRIX_LINKER} linker: ${NYTRIX_SELECTED_LINKER}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=${NYTRIX_LINKER}")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=${NYTRIX_LINKER}")
  elseif (NOT APPLE)
    message(STATUS "Using the compiler's default system linker")
  endif()
endif()
add_compile_definitions($<$<CONFIG:Debug>:DEBUG>)
add_compile_definitions($<$<CONFIG:Release>:NDEBUG>)

# Debug information format
if (WIN32)
  add_compile_options($<$<CONFIG:Debug>:-gcodeview>)
  if (NYTRIX_RELEASE_DEBUG_INFO)
    add_compile_options($<$<CONFIG:Release>:-gcodeview>)
  endif()
else()
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
      # Linker selection may change in an existing build tree. Never reuse a
      # capability result obtained from mold/lld for the system linker.
      unset(NYTRIX_LINKER_HAS_GDB_INDEX CACHE)
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
  # The POSIX compiler raises RLIMIT_STACK for compiler/import frames. PE
  # executables need their reserve set at link time; pages commit on demand.
  if (MSVC)
    add_link_options(/STACK:67108864)
  else()
    add_link_options(-Wl,--stack,67108864)
  endif()
endif()

# Binaries prefer a core bundle beside the executable, then use this path as
# a fallback. Preserve explicit paths supplied by packagers. Accept the old
# std.ny cache variable as a migration aid, but always canonicalize its bundle
# filename because the installed source bundle is now core.ny.
if (NOT DEFINED NYTRIX_CORE_PATH OR NYTRIX_CORE_PATH STREQUAL "")
  if (DEFINED NYTRIX_STD_PATH AND NOT NYTRIX_STD_PATH STREQUAL "")
    get_filename_component(_nytrix_legacy_core_dir "${NYTRIX_STD_PATH}" DIRECTORY)
    if (_nytrix_legacy_core_dir STREQUAL "")
      set(_nytrix_legacy_core_dir ".")
    endif()
    set(NYTRIX_CORE_PATH "${_nytrix_legacy_core_dir}/core.ny")
    message(DEPRECATION
      "NYTRIX_STD_PATH is deprecated. Use -DNYTRIX_CORE_PATH=${NYTRIX_CORE_PATH}; the bundle is named core.ny.")
  else()
    set(NYTRIX_CORE_PATH "${CMAKE_INSTALL_FULL_DATADIR}/nytrix/core.ny")
  endif()
endif()
string(REPLACE "\\" "/" NYTRIX_CORE_PATH "${NYTRIX_CORE_PATH}")

if (NYTRIX_USE_LLVM)
  add_library(nytrix_compiler OBJECT ${SRC_COMPILER})
  set_property(TARGET nytrix_compiler PROPERTY POSITION_INDEPENDENT_CODE ON)
  add_dependencies(nytrix_compiler nytrix_version_header)
  nytrix_apply_compile_perf_flags(nytrix_compiler)
  nytrix_apply_size_flags(nytrix_compiler)
  target_include_directories(nytrix_compiler PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/code/lang/runtime"
    "${NYTRIX_ROOT}/src/include"
    "${CMAKE_BINARY_DIR}/include"
    "${CMAKE_BINARY_DIR}"
  )
  if (NYTRIX_LLVM_INCLUDE)
    target_include_directories(nytrix_compiler SYSTEM PRIVATE "${NYTRIX_LLVM_INCLUDE}")
  endif()
  if (NYTRIX_CLANG_INCLUDE)
    target_include_directories(nytrix_compiler SYSTEM PRIVATE "${NYTRIX_CLANG_INCLUDE}")
  endif()
  if (NYTRIX_Z3_FOUND)
    target_include_directories(nytrix_compiler SYSTEM PRIVATE "${NYTRIX_Z3_INCLUDE_DIR}")
    target_compile_definitions(nytrix_compiler PRIVATE NYTRIX_HAS_Z3=1)
  endif()
  target_compile_definitions(nytrix_compiler PRIVATE
    __STDC_CONSTANT_MACROS
    __STDC_FORMAT_MACROS
    __STDC_LIMIT_MACROS
    VERBOSE_BUILD
    NYTRIX_CORE_PATH="${NYTRIX_CORE_PATH}"
    NYTRIX_INSTALL_DATADIR="${CMAKE_INSTALL_DATADIR}"
  )
  if (NYTRIX_HAS_LLVM_CONSTANT_PTR_AUTH_VALUE_KIND)
    target_compile_definitions(nytrix_compiler PRIVATE
      NYTRIX_HAS_LLVM_CONSTANT_PTR_AUTH_VALUE_KIND=1
    )
  endif()
  if (UNIX AND NOT APPLE)
    target_compile_definitions(nytrix_compiler PRIVATE _GNU_SOURCE)
  endif()
  if (WIN32)
    target_compile_definitions(nytrix_compiler PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
    if (CMAKE_CROSSCOMPILING AND EXISTS "${WIN_HEADERS_DIR}/BaseTsd.h")
      target_include_directories(nytrix_compiler PRIVATE "${WIN_HEADERS_DIR}")
    endif()
  endif()
  if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_WARNING_FLAGS})
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_CLANG_WARNING_FLAGS})
    endif()
    target_compile_options(nytrix_compiler PRIVATE -fno-strict-aliasing)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_compile_options(nytrix_compiler PRIVATE -fno-semantic-interposition -fno-plt)
    endif()
  elseif (MSVC)
    target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_WARNING_FLAGS})
  endif()
  if (NYTRIX_LLVM_CFLAGS_LIST)
    target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_LLVM_CFLAGS_LIST})
  endif()
  if (NYTRIX_HOST_CFLAGS)
    target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_HOST_CFLAGS})
  endif()
  if (WIN32 AND NYTRIX_WINSDK_CFLAGS)
    target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_WINSDK_CFLAGS})
  endif()
else()
  set(_nytrix_nollvm_src ${SRC_COMPILER})
  list(FILTER _nytrix_nollvm_src EXCLUDE REGEX "backend/llvm")
  list(FILTER _nytrix_nollvm_src EXCLUDE REGEX "lang/llvm")
  list(FILTER _nytrix_nollvm_src EXCLUDE REGEX "native/llvm")
  list(FILTER _nytrix_nollvm_src EXCLUDE REGEX "ffi/clang\\.c$")
  list(FILTER _nytrix_nollvm_src EXCLUDE REGEX "cmd/ny/repl")
  list(FILTER _nytrix_nollvm_src EXCLUDE REGEX "runtime/")
  list(FILTER _nytrix_nollvm_src EXCLUDE REGEX "wire/cache\\.c$")

  add_library(nytrix_compiler OBJECT ${_nytrix_nollvm_src})
  set_property(TARGET nytrix_compiler PROPERTY POSITION_INDEPENDENT_CODE ON)
  add_dependencies(nytrix_compiler nytrix_version_header)
  nytrix_apply_compile_perf_flags(nytrix_compiler)
  nytrix_apply_size_flags(nytrix_compiler)
  target_include_directories(nytrix_compiler PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/code"
    "${NYTRIX_ROOT}/src/code/lang/types"
    "${NYTRIX_ROOT}/src/code/lang/runtime"
    "${NYTRIX_ROOT}/src/base"
    "${NYTRIX_ROOT}/src/include"
    "${CMAKE_BINARY_DIR}/include"
    "${CMAKE_BINARY_DIR}"
  )
  target_compile_definitions(nytrix_compiler PRIVATE
    NYTRIX_RUNTIME_ONLY=1
    NYTRIX_USE_LLVM=0
    __STDC_CONSTANT_MACROS
    __STDC_FORMAT_MACROS
    __STDC_LIMIT_MACROS
    NYTRIX_CORE_PATH="${NYTRIX_CORE_PATH}"
    NYTRIX_INSTALL_DATADIR="${CMAKE_INSTALL_DATADIR}"
  )
  if (UNIX AND NOT APPLE)
    target_compile_definitions(nytrix_compiler PRIVATE _GNU_SOURCE)
  endif()
  if (WIN32)
    target_compile_definitions(nytrix_compiler PRIVATE WIN32_LEAN_AND_MEAN
      _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
  endif()
  if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_WARNING_FLAGS})
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(nytrix_compiler PRIVATE ${NYTRIX_CLANG_WARNING_FLAGS})
    endif()
    target_compile_options(nytrix_compiler PRIVATE -fno-strict-aliasing -ffunction-sections -fdata-sections)
  endif()

endif()

add_library(nytrix_runtime OBJECT "${RT_INIT}")
add_dependencies(nytrix_runtime nytrix_version_header)
nytrix_apply_compile_perf_flags(nytrix_runtime)
nytrix_apply_size_flags(nytrix_runtime)
target_include_directories(nytrix_runtime PRIVATE
  "${NYTRIX_ROOT}/src"
  "${NYTRIX_ROOT}/src/include"
  "${NYTRIX_ROOT}/src/code/lang/runtime"
  "${CMAKE_BINARY_DIR}"
)
if (NYTRIX_USE_LLVM AND NYTRIX_LLVM_INCLUDE)
  target_include_directories(nytrix_runtime SYSTEM PRIVATE "${NYTRIX_LLVM_INCLUDE}")
endif()
if (NYTRIX_USE_LLVM AND NYTRIX_CLANG_INCLUDE)
  target_include_directories(nytrix_runtime SYSTEM PRIVATE "${NYTRIX_CLANG_INCLUDE}")
endif()
target_compile_definitions(nytrix_runtime PRIVATE
  __STDC_CONSTANT_MACROS
  __STDC_FORMAT_MACROS
  __STDC_LIMIT_MACROS
  VERBOSE_BUILD
  NYTRIX_CORE_PATH="${NYTRIX_CORE_PATH}"
  NYTRIX_INSTALL_DATADIR="${CMAKE_INSTALL_DATADIR}"
)
if (UNIX AND NOT APPLE)
  target_compile_definitions(nytrix_runtime PRIVATE _GNU_SOURCE)
endif()
if (WIN32)
  target_compile_definitions(nytrix_runtime PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
  if (CMAKE_CROSSCOMPILING AND EXISTS "${WIN_HEADERS_DIR}/BaseTsd.h")
    target_include_directories(nytrix_runtime PRIVATE "${WIN_HEADERS_DIR}")
  endif()
endif()
if (NYTRIX_ENABLE_TEST_MODE)
  target_compile_definitions(nytrix_runtime PRIVATE NYTRIX_TEST_MODE)
endif()
if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_WARNING_FLAGS})
  if (CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_CLANG_WARNING_FLAGS})
  endif()
  target_compile_options(nytrix_runtime PRIVATE -fno-strict-aliasing)
  if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_options(nytrix_runtime PRIVATE -fno-semantic-interposition -fno-plt)
    target_compile_options(nytrix_runtime PRIVATE -msha)
  endif()
elseif (MSVC)
  target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_WARNING_FLAGS})
endif()
if (NYTRIX_RUNTIME_O3 AND NOT MSVC)
  target_compile_options(nytrix_runtime PRIVATE $<$<CONFIG:Release>:-O3>)
endif()
if (NYTRIX_USE_LLVM AND NYTRIX_LLVM_CFLAGS_LIST)
  target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_LLVM_CFLAGS_LIST})
endif()
if (NYTRIX_HOST_CFLAGS)
  target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_HOST_CFLAGS})
endif()
if (WIN32 AND NYTRIX_WINSDK_CFLAGS)
  target_compile_options(nytrix_runtime PRIVATE ${NYTRIX_WINSDK_CFLAGS})
endif()

if (NYTRIX_USE_LLVM)
  # Full compiler (LLVM/Z3/clang). The thin `ny` launcher routes native C
  # developer tools to their standalone binaries before entering this target.
  # OUTPUT_NAME ny-full so the launcher can exec it for compiler work.
  add_executable(ny-full
    "${NYTRIX_ROOT}/src/cmd/ny/main.c"
    "${NYTRIX_ROOT}/src/cmd/ny/pkg.c"
    "${NYTRIX_ROOT}/src/cmd/ny/test.c"
    $<TARGET_OBJECTS:nytrix_compiler>
    $<TARGET_OBJECTS:nytrix_runtime>
  )
  include("${NYTRIX_ROOT}/src/code/backend/ir/test/opt/run.cmake")
  add_dependencies(ny-full nytrix_version_header)
  target_include_directories(ny-full PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/cmd/tools"
    "${NYTRIX_ROOT}/src/code/lang/runtime"
    "${NYTRIX_ROOT}/src/include"
    "${CMAKE_BINARY_DIR}"
  )
  if (WIN32)
    target_include_directories(ny-full PRIVATE "${WIN_HEADERS_DIR}")
  endif()
  if (NYTRIX_LLVM_INCLUDE)
    target_include_directories(ny-full SYSTEM PRIVATE "${NYTRIX_LLVM_INCLUDE}")
  endif()
  if (NYTRIX_CLANG_INCLUDE)
    target_include_directories(ny-full SYSTEM PRIVATE "${NYTRIX_CLANG_INCLUDE}")
  endif()
  if (NYTRIX_Z3_FOUND)
    target_link_libraries(ny-full PRIVATE "${NYTRIX_Z3_LIBRARY}")
  endif()
  set_target_properties(ny-full PROPERTIES
    ENABLE_EXPORTS ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    OUTPUT_NAME "ny-full"
    OUTPUT_NAME_DEBUG "ny-full_debug"
  )
  if(UNIX AND NOT APPLE AND EXISTS "${NYTRIX_ROOT}/build/vendor/lib/host")
    # Some vendored compiler libraries (notably libclang) have their own
    # shared-library dependencies such as libedit/libxml2.  DT_RUNPATH is not
    # used for transitive dependency lookup, so prefer the legacy DT_RPATH for
    # build-tree tools: it lets a freshly built ny-full run directly without
    # requiring the ./make wrapper to inject LD_LIBRARY_PATH.
    set_property(TARGET ny-full PROPERTY BUILD_RPATH "$ORIGIN/../vendor/lib/host")
    target_link_options(ny-full PRIVATE "-Wl,--disable-new-dtags")
  endif()
  if (WIN32)
    # Match ELF -rdynamic: JIT-emitted runtime calls include internal
    # helpers that are intentionally absent from the public FFI registry.
    set_target_properties(ny-full PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    if (NOT MSVC)
      target_link_options(ny-full PRIVATE -Wl,--export-all-symbols)
    endif()
    # Set PDB properties for Windows builds
    set_target_properties(ny-full PROPERTIES
      PDB_NAME "$<TARGET_FILE_BASE_NAME:ny-full>.pdb"
      PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
      set_target_properties(ny-full PROPERTIES PDB_NAME "ny-full_debug.pdb")
    endif()
  endif()
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(ny-full PROPERTIES OUTPUT_NAME "ny-full_debug")
  endif()
  if (MSVC)
    target_link_options(ny-full PRIVATE
      $<$<CONFIG:Debug>:-Xlinker /DEBUG>
      $<$<CONFIG:Release>:-Xlinker /DEBUG>
    )
  elseif(APPLE)
    target_compile_options(ny-full PRIVATE -fPIE)
    target_link_options(ny-full PRIVATE -Wl,-pie)
  endif()
  nytrix_apply_compile_perf_flags(ny-full)
  nytrix_apply_link_perf_flags(ny-full)
  nytrix_apply_size_flags(ny-full)
  if (UNIX AND NOT APPLE)
    target_compile_definitions(ny-full PRIVATE _GNU_SOURCE)
  endif()
  # ICF: Identical Code Folding — merge identical functions at link time
  string(TOLOWER "${NYTRIX_ICF_MODE}" _ny_icf_mode)
  if (_ny_icf_mode STREQUAL "safe")
    target_link_options(ny-full PRIVATE -Wl,--icf=safe)
  elseif(_ny_icf_mode STREQUAL "all")
    target_link_options(ny-full PRIVATE -Wl,--icf=all)
  endif()
else()
  add_custom_target(ny-full)
endif()

# Thin launcher: pure --native-only -c arithmetic never loads LLVM (~sub-ms).
add_executable(ny
  "${NYTRIX_ROOT}/src/cmd/ny/launcher.c"
  "${NYTRIX_ROOT}/src/cmd/ny/pureexpr.c"
  "${NYTRIX_ROOT}/src/base/version.c"
)
if (NYTRIX_USE_LLVM)
  add_dependencies(ny ny-full)
endif()
add_dependencies(ny nytrix_version_header)
target_include_directories(ny PRIVATE "${NYTRIX_ROOT}/src" "${CMAKE_BINARY_DIR}")
set_target_properties(ny PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  OUTPUT_NAME_DEBUG "ny_debug"
)
# The native checker object emitter currently produces ELF, not Mach-O.
if (NYTRIX_BUILD_SHARED AND UNIX AND NOT APPLE AND NYTRIX_USE_LLVM AND
    CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|aarch64|arm64)$")
  find_library(NYTRIX_CORE_BC_CHECKER_CRYPTO_LIBRARY NAMES crypto)
  if(NYTRIX_CORE_BC_CHECKER_CRYPTO_LIBRARY)
    set(NYTRIX_CORE_BC_CHECKER
      "${CMAKE_BINARY_DIR}/ny-core-bc-check${CMAKE_EXECUTABLE_SUFFIX}")
    set(NYTRIX_CORE_BC_CHECKER_OBJECT
      "${CMAKE_BINARY_DIR}/ny-core-bc-check.o")
  endif()
endif()

if (BUILD_TESTING)
  function(nytrix_add_nshape_test test_name fixture)
    set(test_timeout 120)
    if(ARGC GREATER 2)
      set(test_timeout "${ARGV2}")
    endif()
    add_test(
      NAME "${test_name}"
      COMMAND "$<TARGET_FILE:ny-test>" --bin "$<TARGET_FILE:ny>"
        "${NYTRIX_ROOT}/${fixture}"
    )
    set_tests_properties("${test_name}" PROPERTIES
      WORKING_DIRECTORY "${NYTRIX_ROOT}"
      TIMEOUT "${test_timeout}"
      ENVIRONMENT
        "NYTRIX_ROOT=${NYTRIX_ROOT};NYTRIX_CMAKE_COMMAND=${CMAKE_COMMAND}"
    )
  endfunction()
endif()

if (BUILD_TESTING AND NYTRIX_USE_LLVM)
  nytrix_add_nshape_test(
    repl-cold-stdlib-imports
    etc/tests/code/probes/repl/repl-cold-imports.nshape
    240
  )
  if(UNIX)
    set_property(TEST repl-cold-stdlib-imports APPEND PROPERTY ENVIRONMENT
      "NYTRIX_TEST_UNIX=1"
    )
    nytrix_add_nshape_test(
      repl-cold-process-tube
      etc/tests/code/probes/repl/repl-cold-process-tube.nshape
    )
    set_property(TEST repl-cold-process-tube APPEND PROPERTY ENVIRONMENT
      "NYTRIX_TEST_UNIX=1"
    )
  endif()
  nytrix_add_nshape_test(
    ny-cold-start-budget
    etc/tests/code/probes/cold-start-budget.nshape
    900
  )
  nytrix_add_nshape_test(
    repl-doc-resolution
    etc/tests/code/probes/repl/repl-doc-resolution.nshape
    60
  )
  nytrix_add_nshape_test(
    repl-run-state
    etc/tests/code/probes/repl/repl-run-state.nshape
    60
  )
  nytrix_add_nshape_test(
    repl-snapshot-state
    etc/tests/code/probes/repl/repl-snapshot-state.nshape
    60
  )
  set_property(TEST repl-doc-resolution repl-run-state repl-snapshot-state
    APPEND PROPERTY ENVIRONMENT
      "NYTRIX_TEST_NO_NATIVE_CACHE=1;NYTRIX_JIT_CACHE=0;NYTRIX_AOT_CACHE=0;NYTRIX_STD_CACHE=0;NYTRIX_STD_BC_CACHE_AUTO=0"
  )
  nytrix_add_nshape_test(
    semantic-effect-artifact
    etc/tests/lang/comptime/reflection/semantic-effect-artifact.nshape
    360
  )
  set_property(TEST semantic-effect-artifact APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_BUILD_ROOT=${CMAKE_BINARY_DIR};NYTRIX_TEST_CORE_BC=${CORE_BC}"
  )
  nytrix_add_nshape_test(
    refinement-interface
    etc/tests/lang/comptime/reflection/refinement-interface.nshape
  )
  nytrix_add_nshape_test(
    effect-interface
    etc/tests/lang/comptime/reflection/effect-interface.nshape
  )
  nytrix_add_nshape_test(
    legacy-derive-expansion
    etc/tests/code/probes/tools/legacy-derive-expansion.nshape
  )
  nytrix_add_nshape_test(
    module-cache-equivalence
    etc/tests/code/probes/cache/module-cache-equivalence.nshape
    600
  )
  nytrix_add_nshape_test(
    std-source-cache-reuse
    etc/tests/code/probes/cache/std-source-cache-reuse.nshape
  )
  if(NYTRIX_CORE_BC_CHECKER)
    nytrix_add_nshape_test(
      core-bc-cache-selector
      etc/tests/code/probes/cache/core-bc-cache-selector.nshape
    )
    set_property(TEST core-bc-cache-selector APPEND PROPERTY ENVIRONMENT
      "NYTRIX_TEST_CORE_BC_CHECKER=${NYTRIX_CORE_BC_CHECKER}"
    )
  endif()
  if(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    nytrix_add_nshape_test(
      native-image-cache
      etc/tests/code/probes/cache/native-image-cache.nshape
      600
    )
    set_property(TEST native-image-cache APPEND PROPERTY ENVIRONMENT
      "NYTRIX_TEST_NO_NATIVE_CACHE=0;NYTRIX_JIT_CACHE=1;NYTRIX_JIT_CACHE_RUN=1;NYTRIX_STENCIL_CACHE=1;NYTRIX_STD_CACHE=1;NYTRIX_TRACE_CACHE=1"
    )
  endif()
  nytrix_add_nshape_test(
    aot-cache-comptime-identity
    etc/tests/code/probes/cache/aot-cache-comptime-identity.nshape
    900
  )
  nytrix_add_nshape_test(
    ny-doc-search-ny-discovery
    etc/tests/code/probes/tools/ny-doc-search-ny-discovery.nshape
  )
  set_property(TEST ny-doc-search-ny-discovery APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_DOC_BIN=$<TARGET_FILE:ny-doc>"
  )
  add_test(
    NAME ny-fmt-cloc
    COMMAND "${CMAKE_COMMAND}"
      "-DNYTRIX_RUN_CLOC_PARITY=ON"
      "-DFMT=$<TARGET_FILE:ny-fmt>"
      "-DNY=$<TARGET_FILE:ny-full>"
      "-DTEST_DIR=${CMAKE_BINARY_DIR}/ny-fmt-cloc"
      -P "${NYTRIX_ROOT}/CMakeLists.txt"
  )
  nytrix_add_nshape_test(
    ny-fmt-format-parity
    etc/tests/code/probes/tools/fmt-format-parity.nshape
  )
  set_property(TEST ny-fmt-format-parity APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
  )
  nytrix_add_nshape_test(
    ny-fmt-structure-parity
    etc/tests/code/probes/tools/fmt-structure-parity.nshape
  )
  set_property(TEST ny-fmt-structure-parity APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
  )
  nytrix_add_nshape_test(
    ny-fmt-cli-parity
    etc/tests/code/probes/tools/fmt-cli-parity.nshape
  )
  set_property(TEST ny-fmt-cli-parity APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
  )
  nytrix_add_nshape_test(
    ny-fmt-operator-migration
    etc/tests/code/probes/tools/fmt-operator-migration.nshape
  )
  nytrix_add_nshape_test(
    ny-operator-surface
    etc/tests/code/probes/tools/operator-surface.nshape
  )
  nytrix_add_nshape_test(
    ny-unified-dispatch-parity
    etc/tests/code/probes/tools/ny-unified-dispatch-parity.nshape
  )
  nytrix_add_nshape_test(
    ny-fmt-audit-methods-parity
    etc/tests/code/probes/tools/fmt-audit-methods-parity.nshape
  )
  set_property(TEST ny-fmt-audit-methods-parity APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
  )
  nytrix_add_nshape_test(
    ny-fmt-variadic-free-parity
    etc/tests/code/probes/tools/fmt-variadic-free-parity.nshape
  )
  set_property(TEST ny-fmt-variadic-free-parity APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
  )
  nytrix_add_nshape_test(
    ny-fmt-repeated-literal-get-parity
    etc/tests/code/probes/tools/fmt-repeated-literal-get-parity.nshape
  )
  set_property(TEST ny-fmt-repeated-literal-get-parity APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
  )
  nytrix_add_nshape_test(
    ny-fmt-case-dispatch-parity
    etc/tests/code/probes/tools/fmt-case-dispatch-parity.nshape
  )
  set_property(TEST ny-fmt-case-dispatch-parity APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
  )
  foreach(_fmt_test IN ITEMS layout-audit trim-pressure)
    nytrix_add_nshape_test(
      ny-fmt-${_fmt_test}-parity
      etc/tests/code/probes/tools/fmt-${_fmt_test}-parity.nshape
    )
    set_property(TEST ny-fmt-${_fmt_test}-parity APPEND PROPERTY ENVIRONMENT
      "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
    )
  endforeach()
  nytrix_add_nshape_test(
    ny-fmt-layout-boundary-parity
    etc/tests/code/probes/tools/fmt-layout-boundary-parity.nshape
    120
  )
  set_property(TEST ny-fmt-layout-boundary-parity APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
  )
  nytrix_add_nshape_test(
    ny-fmt-audit-duplicate-syntax-parity
    etc/tests/code/probes/tools/fmt-audit-duplicate-syntax-parity.nshape
  )
  set_property(TEST ny-fmt-audit-duplicate-syntax-parity APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
  )
  nytrix_add_nshape_test(
    ny-fmt-audit-overwrite-parity
    etc/tests/code/probes/tools/fmt-audit-overwrite-parity.nshape
  )
  set_property(TEST ny-fmt-audit-overwrite-parity APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
  )
  foreach(_fmt_audit_test IN ITEMS
      value-semantics literal-hazards branch-hazards)
    nytrix_add_nshape_test(
      ny-fmt-audit-${_fmt_audit_test}-parity
      etc/tests/code/probes/tools/fmt-audit-${_fmt_audit_test}-parity.nshape
    )
    set_property(TEST ny-fmt-audit-${_fmt_audit_test}-parity APPEND PROPERTY ENVIRONMENT
      "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
    )
  endforeach()
  nytrix_add_nshape_test(
    ny-fmt-c-family-parity
    etc/tests/code/probes/tools/fmt-c-family-parity.nshape
  )
  set_property(TEST ny-fmt-c-family-parity APPEND PROPERTY ENVIRONMENT
    "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
  )
  foreach(_fmt_test IN ITEMS
      c-analysis c-unsafe-audit c-battery-audit audit-corpus)
    nytrix_add_nshape_test(
      ny-fmt-${_fmt_test}-parity
      etc/tests/code/probes/tools/fmt-${_fmt_test}-parity.nshape
      180
    )
    set_property(TEST ny-fmt-${_fmt_test}-parity APPEND PROPERTY ENVIRONMENT
      "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
    )
  endforeach()
  foreach(_fmt_test IN ITEMS c-method-audit c-pressure-audit)
    nytrix_add_nshape_test(
      ny-fmt-${_fmt_test}-parity
      etc/tests/code/probes/tools/fmt-${_fmt_test}-parity.nshape
    )
    set_property(TEST ny-fmt-${_fmt_test}-parity APPEND PROPERTY ENVIRONMENT
      "NYTRIX_TEST_FMT_BIN=$<TARGET_FILE:ny-fmt>"
    )
  endforeach()
endif()
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set_target_properties(ny PROPERTIES OUTPUT_NAME "ny_debug")
endif()
if (APPLE)
  target_compile_options(ny PRIVATE -fPIE)
  target_link_options(ny PRIVATE -Wl,-pie)
endif()
nytrix_apply_compile_perf_flags(ny)
nytrix_apply_size_flags(ny)
if (UNIX AND NOT APPLE)
  target_compile_definitions(ny PRIVATE _GNU_SOURCE)
endif()

function(nytrix_add_tool tool_target tool_name)
  set(_tool_sources
    "${NYTRIX_ROOT}/src/cmd/tools/main.c"
    "${NYTRIX_ROOT}/src/base/args.c"
    "${NYTRIX_ROOT}/src/base/util.c"
    "${NYTRIX_ROOT}/src/base/curl.c"
    "${NYTRIX_ROOT}/src/base/progress.c"
    "${NYTRIX_ROOT}/src/base/debug.c"
  )
  if ("${tool_name}" STREQUAL "fmt")
    list(APPEND _tool_sources
      "${NYTRIX_ROOT}/src/cmd/fmt/init.c"
      "${NYTRIX_ROOT}/src/base/process.c"
    )
  elseif ("${tool_name}" STREQUAL "perf")
    list(APPEND _tool_sources "${NYTRIX_ROOT}/src/cmd/test/perf/perf.c")
  elseif ("${tool_name}" STREQUAL "test")
    list(APPEND _tool_sources
      "${NYTRIX_ROOT}/src/cmd/test/init.c"
      "${NYTRIX_ROOT}/src/cmd/test/fuzz/analyze.c"
      "${NYTRIX_ROOT}/src/cmd/test/fuzz/compare/main.c"
      "${NYTRIX_ROOT}/src/cmd/test/fuzz/generate.c"
      "${NYTRIX_ROOT}/src/cmd/test/fuzz/proc.c"
      "${NYTRIX_ROOT}/src/cmd/test/fuzz/shape.c"
      "${NYTRIX_ROOT}/src/cmd/test/fuzz/synth.c"
      "${NYTRIX_ROOT}/src/cmd/test/fuzz/usage.c"
      "${NYTRIX_ROOT}/src/cmd/test/fuzz/util.c"
      "${NYTRIX_ROOT}/src/cmd/test/fuzz/worker.c"
      "${NYTRIX_ROOT}/src/cmd/test/fuzz/cbridge.c"
    )
  elseif ("${tool_name}" STREQUAL "doc")
    list(APPEND _tool_sources
      "${NYTRIX_ROOT}/src/cmd/web/init.c"
      "${NYTRIX_ROOT}/src/base/process.c"
    )
  elseif ("${tool_name}" STREQUAL "make")
    list(APPEND _tool_sources "${NYTRIX_ROOT}/src/cmd/tools/make/make.c")
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
    target_compile_definitions(${tool_target} PRIVATE
      NY_TOOL_HAS_TEST=1
      NY_TOOL_HAS_FUZZ=1
    )
  elseif ("${tool_name}" STREQUAL "doc")
    target_compile_definitions(${tool_target} PRIVATE NY_TOOL_HAS_WEB=1)
  elseif ("${tool_name}" STREQUAL "make")
    target_compile_definitions(${tool_target} PRIVATE NY_TOOL_HAS_MAKE=1)
  endif()
  target_include_directories(${tool_target} PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/include"
    "${NYTRIX_ROOT}/src/code/lang/runtime"
    "${CMAKE_BINARY_DIR}"
  )
  if (MSVC)
    target_compile_options(${tool_target} PRIVATE /W4)
  elseif (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${tool_target} PRIVATE -Wall -Wextra -Wunused -Wmissing-prototypes)
    if (NYTRIX_STRICT_WARNINGS)
      target_compile_options(${tool_target} PRIVATE -Wpedantic)
    endif()
  endif()
  if (NYTRIX_TOOLS_O3 AND NOT MSVC)
    target_compile_options(${tool_target} PRIVATE $<$<CONFIG:Release>:-O3>)
  endif()
  set_target_properties(${tool_target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )
endfunction()

nytrix_add_tool(ny-fmt "fmt")
nytrix_add_tool(ny-perf "perf")
nytrix_add_tool(ny-test "test")
if (UNIX)
  # bench geomean reporting uses log()/exp().
  target_link_libraries(ny-test PRIVATE m)
endif()
nytrix_add_tool(ny-doc "doc")
nytrix_add_tool(ny-make "make")

if (NYTRIX_BUILD_LSP)
  add_executable(ny-lsp
    "${NYTRIX_ROOT}/src/cmd/tools/lsp/main.c"
    $<TARGET_OBJECTS:nytrix_compiler>
    $<TARGET_OBJECTS:nytrix_runtime>
  )
  add_dependencies(ny-lsp nytrix_version_header)
  target_include_directories(ny-lsp PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/include"
    "${NYTRIX_ROOT}/src/code/lang/runtime"
    "${CMAKE_BINARY_DIR}"
  )
  if (WIN32)
    target_include_directories(ny-lsp PRIVATE "${WIN_HEADERS_DIR}")
  endif()
  if (NYTRIX_LLVM_INCLUDE)
    target_include_directories(ny-lsp SYSTEM PRIVATE "${NYTRIX_LLVM_INCLUDE}")
  endif()
  if (NYTRIX_CLANG_INCLUDE)
    target_include_directories(ny-lsp SYSTEM PRIVATE "${NYTRIX_CLANG_INCLUDE}")
  endif()
  if (NYTRIX_Z3_FOUND)
    target_link_libraries(ny-lsp PRIVATE "${NYTRIX_Z3_LIBRARY}")
  endif()
  set_target_properties(ny-lsp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )
  if(UNIX AND NOT APPLE AND EXISTS "${NYTRIX_ROOT}/build/vendor/lib/host")
    set_property(TARGET ny-lsp PROPERTY BUILD_RPATH "$ORIGIN/../vendor/lib/host")
    target_link_options(ny-lsp PRIVATE "-Wl,--disable-new-dtags")
  endif()
  if (WIN32)
    set_target_properties(ny-lsp PROPERTIES
      ENABLE_EXPORTS ON WINDOWS_EXPORT_ALL_SYMBOLS ON)
    if (NOT MSVC)
      target_link_options(ny-lsp PRIVATE -Wl,--export-all-symbols)
    endif()
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
elseif(NYTRIX_BUILD_LSP)
  add_custom_target(ny-lsp)
endif()

add_executable(ny-dap
  "${NYTRIX_ROOT}/src/cmd/tools/dap/main.c"
  "${NYTRIX_ROOT}/src/base/util.c"
  "${NYTRIX_ROOT}/src/base/curl.c"
  "${NYTRIX_ROOT}/src/base/progress.c"
  "${NYTRIX_ROOT}/src/base/debug.c"
)
target_compile_definitions(ny-dap PRIVATE NY_TOOL_ENTRY="dap")
add_dependencies(ny-dap nytrix_version_header)
target_include_directories(ny-dap PRIVATE
  "${NYTRIX_ROOT}/src"
  "${NYTRIX_ROOT}/src/include"
  "${NYTRIX_ROOT}/src/code/lang/runtime"
  "${CMAKE_BINARY_DIR}"
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

  if (NYTRIX_USE_LLVM)
    target_compile_definitions(ny-full PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
    if (NYTRIX_BUILD_LSP)
      target_compile_definitions(ny-lsp PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
    endif()

    # CFLAGS
    set(_all_cflags ${NYTRIX_LLVM_CFLAGS_LIST} ${NYTRIX_HOST_CFLAGS} ${NYTRIX_WINSDK_CFLAGS})
    foreach(_flag IN LISTS _all_cflags)
      target_compile_options(ny-full PRIVATE "${_flag}")
      if (NYTRIX_BUILD_LSP)
        target_compile_options(ny-lsp PRIVATE "${_flag}")
      endif()
    endforeach()

    # LDFLAGS & LIBS
    target_link_libraries(ny-full PRIVATE ws2_32 user32 kernel32 advapi32 shell32 ole32 uuid "${NYTRIX_CLANG_LIBRARY_FOUND}" ZLIB::ZLIB)
    set(_all_ldflags ${NYTRIX_LLVM_LDFLAGS_LIST} ${NYTRIX_HOST_LDFLAGS} ${NYTRIX_WINSDK_LDFLAGS})
    foreach(_flag IN LISTS _all_ldflags)
      if(_flag MATCHES "^-L" OR _flag MATCHES "^/LIBPATH" OR _flag MATCHES "^-l")
        target_link_options(ny-full PRIVATE "${_flag}")
      else()
        target_link_libraries(ny-full PRIVATE "${_flag}")
      endif()
    endforeach()
    foreach(_lib IN LISTS LLVM_LIBS)
      target_link_libraries(ny-full PRIVATE "${_lib}")
    endforeach()

    if (NYTRIX_BUILD_LSP)
      target_link_libraries(ny-lsp PRIVATE ws2_32 user32 kernel32 advapi32 shell32 ole32 uuid "${NYTRIX_CLANG_LIBRARY_FOUND}" ZLIB::ZLIB)
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
  endif()
  target_link_libraries(ny-doc PRIVATE ws2_32)
  set(_win_targets ny ny-fmt ny-perf ny-test ny-doc ny-make)
  if (NYTRIX_USE_LLVM)
    list(APPEND _win_targets ny-full)
    if (NYTRIX_BUILD_LSP)
      list(APPEND _win_targets ny-lsp)
    endif()
  endif()
  foreach(_target IN LISTS _win_targets)
    nytrix_stage_windows_runtime_deps(${_target})
  endforeach()
else()
  if (NYTRIX_USE_LLVM)
    # CFLAGS
    set(_all_cflags ${NYTRIX_LLVM_CFLAGS_LIST} ${NYTRIX_HOST_CFLAGS})
    foreach(_flag IN LISTS _all_cflags)
      target_compile_options(ny-full PRIVATE "${_flag}")
      if (NYTRIX_BUILD_LSP)
        target_compile_options(ny-lsp PRIVATE "${_flag}")
      endif()
    endforeach()

    # LDFLAGS & LIBS (full compiler only — thin `ny` launcher stays free of LLVM)
    target_link_libraries(ny-full PRIVATE m pthread "${NYTRIX_CLANG_LIBRARY_FOUND}")
    set(_all_ldflags ${NYTRIX_LLVM_LDFLAGS_LIST} ${NYTRIX_HOST_LDFLAGS})
    foreach(_flag IN LISTS _all_ldflags)
      if(_flag MATCHES "^-L" OR _flag MATCHES "^-l")
        target_link_options(ny-full PRIVATE "${_flag}")
      else()
        target_link_libraries(ny-full PRIVATE "${_flag}")
      endif()
    endforeach()
    foreach(_lib IN LISTS LLVM_LIBS)
      target_link_libraries(ny-full PRIVATE "${_lib}")
    endforeach()

    if (UNIX AND NOT APPLE)
      target_link_libraries(ny-full PRIVATE dl util m ZLIB::ZLIB)
    elseif (APPLE)
      target_link_libraries(ny-full PRIVATE ZLIB::ZLIB)
    endif()
    target_link_options(ny-full PRIVATE -rdynamic)

    if (NYTRIX_BUILD_LSP)
      target_link_libraries(ny-lsp PRIVATE m pthread util "${NYTRIX_CLANG_LIBRARY_FOUND}")
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
  else()
    if (NYTRIX_BUILD_LSP)
      target_link_libraries(ny-lsp PRIVATE m pthread util)
      if (UNIX AND NOT APPLE)
        target_link_libraries(ny-lsp PRIVATE dl ZLIB::ZLIB)
        target_link_options(ny-lsp PRIVATE -Wl,--unresolved-symbols=ignore-all)
      elseif (APPLE)
        target_link_libraries(ny-lsp PRIVATE ZLIB::ZLIB)
        target_link_options(ny-lsp PRIVATE -undefined dynamic_lookup)
      endif()
      target_link_options(ny-lsp PRIVATE -rdynamic)
    endif()
  endif()
endif()

if (NYTRIX_USE_LLVM)
  # Standalone regression driver for registered module identity. Link the
  # same compiler/runtime objects and dependency set as ny-full for local
  # loader integration checks on each supported platform.
  add_executable(ny-module-identity-test
    "${NYTRIX_ROOT}/src/base/loader/tests/module_identity.c"
    $<TARGET_OBJECTS:nytrix_compiler>
    $<TARGET_OBJECTS:nytrix_runtime>
  )
  add_dependencies(ny-module-identity-test nytrix_version_header std)
  target_include_directories(ny-module-identity-test PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/include"
    "${NYTRIX_ROOT}/src/code/lang/runtime"
    "${CMAKE_BINARY_DIR}"
  )
  get_target_property(_ny_identity_compile_defs ny-full COMPILE_DEFINITIONS)
  if (_ny_identity_compile_defs)
    target_compile_definitions(ny-module-identity-test PRIVATE
      ${_ny_identity_compile_defs})
  endif()
  get_target_property(_ny_identity_compile_opts ny-full COMPILE_OPTIONS)
  if (_ny_identity_compile_opts)
    target_compile_options(ny-module-identity-test PRIVATE
      ${_ny_identity_compile_opts})
  endif()
  get_target_property(_ny_identity_link_libs ny-full LINK_LIBRARIES)
  if (_ny_identity_link_libs)
    target_link_libraries(ny-module-identity-test PRIVATE
      ${_ny_identity_link_libs})
  endif()
  get_target_property(_ny_identity_link_opts ny-full LINK_OPTIONS)
  if (_ny_identity_link_opts)
    target_link_options(ny-module-identity-test PRIVATE
      ${_ny_identity_link_opts})
  endif()
  set_target_properties(ny-module-identity-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )
  nytrix_apply_compile_perf_flags(ny-module-identity-test)
  nytrix_apply_link_perf_flags(ny-module-identity-test)
  nytrix_apply_size_flags(ny-module-identity-test)
  if (UNIX AND NOT APPLE)
    target_compile_definitions(ny-module-identity-test PRIVATE _GNU_SOURCE)
  endif()
  if (WIN32)
    target_compile_definitions(ny-module-identity-test PRIVATE
      _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS PATH_MAX=4096)
    nytrix_stage_windows_runtime_deps(ny-module-identity-test)
  endif()

  # Exercise large-function backend scaling in the normal POSIX test path.
  # The driver builds verified 10k+ instruction NYIR functions and times
  # lowering, register allocation, and object emission independently.
  if (UNIX)
    add_executable(ny-backend-scaling-test
      "${NYTRIX_ROOT}/src/code/backend/ir/test/opt/backend_monolithic_scaling.c"
      $<TARGET_OBJECTS:nytrix_compiler>
      $<TARGET_OBJECTS:nytrix_runtime>
    )
    add_dependencies(ny-backend-scaling-test nytrix_version_header std)
    target_include_directories(ny-backend-scaling-test PRIVATE
      "${NYTRIX_ROOT}/src"
      "${NYTRIX_ROOT}/src/code/lang/runtime"
      "${NYTRIX_ROOT}/src/include"
      "${CMAKE_BINARY_DIR}"
    )
    get_target_property(_ny_scaling_compile_defs ny-full COMPILE_DEFINITIONS)
    if (_ny_scaling_compile_defs)
      target_compile_definitions(ny-backend-scaling-test PRIVATE
        ${_ny_scaling_compile_defs})
    endif()
    get_target_property(_ny_scaling_compile_opts ny-full COMPILE_OPTIONS)
    if (_ny_scaling_compile_opts)
      target_compile_options(ny-backend-scaling-test PRIVATE
        ${_ny_scaling_compile_opts})
    endif()
    get_target_property(_ny_scaling_link_libs ny-full LINK_LIBRARIES)
    if (_ny_scaling_link_libs)
      target_link_libraries(ny-backend-scaling-test PRIVATE
        ${_ny_scaling_link_libs})
    endif()
    get_target_property(_ny_scaling_link_opts ny-full LINK_OPTIONS)
    if (_ny_scaling_link_opts)
      target_link_options(ny-backend-scaling-test PRIVATE
        ${_ny_scaling_link_opts})
    endif()
    set_target_properties(ny-backend-scaling-test PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    nytrix_apply_compile_perf_flags(ny-backend-scaling-test)
    nytrix_apply_link_perf_flags(ny-backend-scaling-test)
    nytrix_apply_size_flags(ny-backend-scaling-test)
    if (NOT APPLE)
      target_compile_definitions(ny-backend-scaling-test PRIVATE _GNU_SOURCE)
    endif()

    # Keep the semantics driver independently runnable so its bounded,
    # operation-specific TV selectors do not require the multi-minute
    # aggregate optimizer suite.
    add_executable(ny-tv-smt-test
      "${NYTRIX_ROOT}/src/code/backend/ir/test/opt/tv_smt_semantics.c"
      $<TARGET_OBJECTS:nytrix_compiler>
      $<TARGET_OBJECTS:nytrix_runtime>
    )
    add_dependencies(ny-tv-smt-test nytrix_version_header std)
    target_include_directories(ny-tv-smt-test PRIVATE
      "${NYTRIX_ROOT}/src"
      "${NYTRIX_ROOT}/src/code/lang/runtime"
      "${NYTRIX_ROOT}/src/include"
      "${CMAKE_BINARY_DIR}"
    )
    get_target_property(_ny_tv_compile_defs ny-full COMPILE_DEFINITIONS)
    if (_ny_tv_compile_defs)
      target_compile_definitions(ny-tv-smt-test PRIVATE ${_ny_tv_compile_defs})
    endif()
    get_target_property(_ny_tv_compile_opts ny-full COMPILE_OPTIONS)
    if (_ny_tv_compile_opts)
      target_compile_options(ny-tv-smt-test PRIVATE ${_ny_tv_compile_opts})
    endif()
    get_target_property(_ny_tv_link_libs ny-full LINK_LIBRARIES)
    if (_ny_tv_link_libs)
      target_link_libraries(ny-tv-smt-test PRIVATE ${_ny_tv_link_libs})
    endif()
    get_target_property(_ny_tv_link_opts ny-full LINK_OPTIONS)
    if (_ny_tv_link_opts)
      target_link_options(ny-tv-smt-test PRIVATE ${_ny_tv_link_opts})
    endif()
    set_target_properties(ny-tv-smt-test PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/driver"
    )
    nytrix_apply_compile_perf_flags(ny-tv-smt-test)
    nytrix_apply_link_perf_flags(ny-tv-smt-test)
    nytrix_apply_size_flags(ny-tv-smt-test)
    if (NOT APPLE)
      target_compile_definitions(ny-tv-smt-test PRIVATE _GNU_SOURCE)
    endif()
  endif()
endif()

if (NYTRIX_BUILD_SHARED AND NOT WIN32 AND NYTRIX_USE_LLVM)
  add_library(nytrixrt SHARED "${RT_INIT}" $<TARGET_OBJECTS:nytrix_compiler>)
  add_dependencies(nytrixrt nytrix_version_header)
  add_dependencies(nytrixrt std)
  nytrix_apply_size_flags(nytrixrt)
  if (UNIX AND NOT APPLE)
    # Bind the shared library's internal `rt_*` references to its own
    # definitions.  The executable (ny-full) embeds a static runtime copy,
    # and without -Bsymbolic the JIT's dlsym/lazy PLT resolution can route
    # runtime helper calls through the executable's copy, which is built
    # with a different macro set and silently misbehaves (intermittent
    # SIGSEGVs in rt_native_tag_of / rt_native_tbuf_new).
    target_link_options(nytrixrt PRIVATE -Wl,-Bsymbolic)
  endif()
  # CFLAGS
  set(_all_cflags ${NYTRIX_LLVM_CFLAGS_LIST} ${NYTRIX_HOST_CFLAGS})
  foreach(_flag IN LISTS _all_cflags)
    target_compile_options(nytrixrt PRIVATE "${_flag}")
  endforeach()

  target_include_directories(nytrixrt PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/include"
    "${NYTRIX_ROOT}/src/code/lang/runtime"
    "${CMAKE_BINARY_DIR}"
  )
  if (NYTRIX_LLVM_INCLUDE)
    target_include_directories(nytrixrt PRIVATE "${NYTRIX_LLVM_INCLUDE}")
  endif()
  if (NYTRIX_CLANG_INCLUDE)
    target_include_directories(nytrixrt PRIVATE "${NYTRIX_CLANG_INCLUDE}")
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
    NYTRIX_CORE_PATH="${NYTRIX_CORE_PATH}"
    NYTRIX_INSTALL_DATADIR="${CMAKE_INSTALL_DATADIR}"
  )

  # LDFLAGS & LIBS
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
  target_link_libraries(nytrixrt PRIVATE "${NYTRIX_CLANG_LIBRARY_FOUND}")
  if (UNIX AND NOT APPLE)
    target_compile_definitions(nytrixrt PRIVATE _GNU_SOURCE)
    target_link_libraries(nytrixrt PRIVATE dl ZLIB::ZLIB)
  elseif (APPLE)
    target_link_libraries(nytrixrt PRIVATE ZLIB::ZLIB)
  endif()

  if(BUILD_TESTING)
    add_test(NAME nytrix-embed-api
      COMMAND ny-full --nytrix-embed-test)
    set_tests_properties(nytrix-embed-api PROPERTIES TIMEOUT 60)
    add_executable(nytrix-embed-gc-handles
      "${NYTRIX_ROOT}/src/code/lang/runtime/tests/embed_handle.c")
    target_include_directories(nytrix-embed-gc-handles PRIVATE
      "${NYTRIX_ROOT}/src"
      "${NYTRIX_ROOT}/src/code/lang/runtime"
      "${NYTRIX_ROOT}/src/include")
    target_compile_options(nytrix-embed-gc-handles PRIVATE
      -Wall -Wextra -Werror)
    target_link_libraries(nytrix-embed-gc-handles PRIVATE nytrixrt)
    add_test(NAME nytrix-embed-gc-handles COMMAND nytrix-embed-gc-handles)
    set_tests_properties(nytrix-embed-gc-handles PROPERTIES ENVIRONMENT
      "NYTRIX_GC=1;NYTRIX_GC_NURSERY_SIZE=64k;NYTRIX_GC_TENURED_SIZE=256k")
  endif()
endif()

if (NYTRIX_CORE_BC_CHECKER)
  add_custom_command(
    OUTPUT "${NYTRIX_CORE_BC_CHECKER_OBJECT}"
    COMMAND "$<TARGET_FILE:ny-full>" --native-only -O0
      "--output=${NYTRIX_CORE_BC_CHECKER_OBJECT}"
      "${NYTRIX_ROOT}/src/cmd/ny/core_bc_cache.ny"
    DEPENDS ny-full "${NYTRIX_ROOT}/src/cmd/ny/core_bc_cache.ny"
    WORKING_DIRECTORY "${NYTRIX_ROOT}"
    COMMENT "Compiling Ny shared-core cache checker"
    VERBATIM
  )
  set_source_files_properties("${NYTRIX_CORE_BC_CHECKER_OBJECT}" PROPERTIES
    EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_executable(ny-core-bc-checker "${NYTRIX_CORE_BC_CHECKER_OBJECT}")
  set_target_properties(ny-core-bc-checker PROPERTIES LINKER_LANGUAGE C)
  target_link_libraries(ny-core-bc-checker PRIVATE
    nytrixrt "${NYTRIX_CORE_BC_CHECKER_CRYPTO_LIBRARY}")
  set_target_properties(ny-core-bc-checker PROPERTIES
    OUTPUT_NAME "ny-core-bc-check"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_dependencies(ny-test ny-core-bc-checker)
  add_dependencies(ny-make ny-core-bc-checker)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
    CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  file(GLOB NYR_SOURCES CONFIGURE_DEPENDS
    "${NYTRIX_ROOT}/src/base/*.c"
    "${NYTRIX_ROOT}/src/code/backend/ir/*.c"
    "${NYTRIX_ROOT}/src/code/backend/ir/opt/*.c"
    "${NYTRIX_ROOT}/src/code/backend/machine/*.c"
    "${NYTRIX_ROOT}/src/code/backend/object/*.c"
    "${NYTRIX_ROOT}/src/code/backend/target.c"
  )
  list(FILTER NYR_SOURCES EXCLUDE REGEX
    "/(emit|tier|report|oracle|portable|embedding)\\.c$")
  # The C source loader/debug adapter is a frontend service. Keep it out of
  # the neutral NYIR backend library.
  list(FILTER NYR_SOURCES EXCLUDE REGEX "/src/base/(loader|dbg)\\.c$")
  foreach(NYR_SOURCE IN LISTS NYR_SOURCES)
    file(STRINGS "${NYR_SOURCE}" NYR_FRONTEND_INCLUDES
      REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](code/lang/|cmd/ny/|sema/)")
    if (NYR_FRONTEND_INCLUDES)
      message(FATAL_ERROR
        "Frontend dependency in neutral nyr source ${NYR_SOURCE}: ${NYR_FRONTEND_INCLUDES}")
    endif()
  endforeach()
  list(APPEND NYR_SOURCES "${NYTRIX_ROOT}/src/code/backend/abi.c")
  list(APPEND NYR_SOURCES "${NYTRIX_ROOT}/src/code/backend/common.c")
  add_library(nyr STATIC ${NYR_SOURCES})
  target_link_libraries(nyr PUBLIC ${CMAKE_DL_LIBS})
  if(TARGET nytrixrt)
    target_link_libraries(nytrixrt PRIVATE ${CMAKE_DL_LIBS})
  endif()
  set_property(TARGET nyr PROPERTY POSITION_INDEPENDENT_CODE ON)
  target_include_directories(nyr PUBLIC
    "${NYTRIX_ROOT}/src/include"
  )
  target_include_directories(nyr PRIVATE
    "${NYTRIX_ROOT}/src"
    "${CMAKE_BINARY_DIR}/include"
    "${CMAKE_BINARY_DIR}"
  )
  target_compile_definitions(nyr PRIVATE
    NYTRIX_USE_LLVM=0
    NYTRIX_RUNTIME_ONLY=1
    NYR_NO_EVAL=1
    NYTRIX_NO_OPENMP=1
    _GNU_SOURCE
  )
  # The standalone backend library uses the C11-thread fallback so consumers
  # can link libnyr.a without inheriting an OpenMP runtime requirement.
  get_target_property(NYR_LINK_LIBRARIES nyr LINK_LIBRARIES)
  if(NYR_LINK_LIBRARIES)
    list(REMOVE_ITEM NYR_LINK_LIBRARIES OpenMP::OpenMP_C)
    set_property(TARGET nyr PROPERTY LINK_LIBRARIES "${NYR_LINK_LIBRARIES}")
  endif()
  if(EXISTS "${NYTRIX_ROOT}/src/code/backend/ir/test/public_acceptance.c")
    set(NYR_PUBLIC_ACCEPTANCE_SOURCE
      "${NYTRIX_ROOT}/src/code/backend/ir/test/public_acceptance.c")
    add_executable(nyr-backend-ir-acceptance
      "${NYR_PUBLIC_ACCEPTANCE_SOURCE}")
    target_link_libraries(nyr-backend-ir-acceptance PRIVATE nyr m)
    if(BUILD_TESTING)
      add_test(NAME nyr-backend-ir-acceptance
        COMMAND nyr-backend-ir-acceptance)
      add_custom_target(nyr-backend-ir-acceptance-test
        COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
          -R "^nyr-backend-ir-acceptance$"
        DEPENDS nyr-backend-ir-acceptance
        USES_TERMINAL
        VERBATIM)
    endif()
    if(BUILD_TESTING AND TARGET nytrixrt)
      add_executable(nyr-shared-api-acceptance
        "${NYR_PUBLIC_ACCEPTANCE_SOURCE}")
      target_include_directories(nyr-shared-api-acceptance PRIVATE
        "${NYTRIX_ROOT}/src/include")
      target_link_libraries(nyr-shared-api-acceptance PRIVATE nytrixrt m)
      add_test(NAME nyr-shared-api-acceptance
        COMMAND nyr-shared-api-acceptance)
      set_tests_properties(nyr-shared-api-acceptance PROPERTIES TIMEOUT 30)
    endif()
  endif()
  if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(nyr PRIVATE
      ${NYTRIX_WARNING_FLAGS}
      -fno-strict-aliasing
      -ffunction-sections
      -fdata-sections
    )
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(nyr PRIVATE ${NYTRIX_CLANG_WARNING_FLAGS})
    endif()
  endif()

  add_executable(nyc "${NYTRIX_ROOT}/src/code/lang/c/nyc.c")
  add_dependencies(nyc nyr)
  target_include_directories(nyc PRIVATE
    "${NYTRIX_ROOT}/src"
    "${NYTRIX_ROOT}/src/include"
    "${CMAKE_BINARY_DIR}/include"
  )
  target_compile_options(nyc PRIVATE -Wall -Wextra -Werror)
  target_link_options(nyc PRIVATE -Wl,--gc-sections)
  target_sources(nyc PRIVATE
    "${NYTRIX_ROOT}/src/code/lang/c/c_lex.c"
    "${NYTRIX_ROOT}/src/code/lang/c/c_abi.c"
    "${NYTRIX_ROOT}/src/code/lang/c/nyc_frontend.c"
  )
  target_link_libraries(nyc PRIVATE nyr m)
  if(BUILD_TESTING)
    find_program(NYR_LLD_LINK_EXECUTABLE NAMES lld-link)
    find_program(NYR_LD64_LLD_EXECUTABLE NAMES ld64.lld)
    set(NYR_C_TEST_DIR "${NYTRIX_ROOT}/src/code/lang/c/tests")
    set(NYR_C_TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/nyr-c-tests")
    add_test(
      NAME nyr-c-frontend
      COMMAND "${CMAKE_COMMAND}"
        "-DNYC=$<TARGET_FILE:nyc>"
        "-DCC=${CMAKE_C_COMPILER}"
        "-DSOURCE_DIR=${NYR_C_TEST_DIR}"
        "-DBINARY_DIR=${NYR_C_TEST_OUTPUT_DIR}"
        "-DLLD_LINK=${NYR_LLD_LINK_EXECUTABLE}"
        "-DLD64_LLD=${NYR_LD64_LLD_EXECUTABLE}"
        -P "${NYR_C_TEST_DIR}/run.cmake"
    )
    set_tests_properties(nyr-c-frontend PROPERTIES TIMEOUT 120)
    add_executable(ny-c-lexer-test
      "${NYR_C_TEST_DIR}/lexer.c"
      "${NYTRIX_ROOT}/src/code/lang/c/c_lex.c"
    )
    target_include_directories(ny-c-lexer-test PRIVATE
      "${NYTRIX_ROOT}/src"
    )
    target_compile_options(ny-c-lexer-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME nyr-c-lexer COMMAND ny-c-lexer-test)
    find_program(NYR_WIN64_CC NAMES x86_64-w64-mingw32-gcc)
    find_program(NYR_WIN64_RUNNER NAMES wine wine64)
    if(NYR_WIN64_CC AND NYR_WIN64_RUNNER)
      nytrix_add_nshape_test(
        nyr-ny-full-win64-aggregate
        etc/tests/code/formats/coff64/win64-aggregate-runtime.nshape
        120
      )
      set_property(TEST nyr-ny-full-win64-aggregate APPEND PROPERTY ENVIRONMENT
        "NYTRIX_TEST_WIN64_CC=${NYR_WIN64_CC};NYTRIX_TEST_WIN64_RUNNER=${NYR_WIN64_RUNNER}"
      )
    endif()
    add_custom_target(nyc-test
      COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -R "^nyr-c-frontend$"
      DEPENDS nyc
      USES_TERMINAL
      VERBATIM
    )
  endif()
endif()

if (NYTRIX_USE_LLVM)
  install(TARGETS ny ny-full
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
else()
  install(TARGETS ny
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()

# Standalone tool targets remain buildable for developer compatibility.
# Installed tooling is exposed through `ny fmt|perf|test|doc|make|pkg`.
if (NYTRIX_BUILD_LSP)
  install(TARGETS ny-lsp
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
    CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  install(TARGETS nyr
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
  install(TARGETS nyc
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()
if (NYTRIX_BUILD_SHARED AND NYTRIX_USE_LLVM AND
    CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
    CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  install(TARGETS nytrixrt
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
endif()
install(DIRECTORY "${NYTRIX_ROOT}/src/include/"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h"
)
# Keep the installed bundle name aligned with NYTRIX_CORE_PATH above. The
# legacy NYTRIX_STD_PATH environment override remains supported at runtime,
# but CMake never points binaries at the retired std.ny filename.
install(FILES "${CORE_NY}"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/nytrix
  RENAME core.ny
)
install(DIRECTORY "${NYTRIX_ROOT}/lib" DESTINATION ${CMAKE_INSTALL_DATADIR}/nytrix
  FILES_MATCHING PATTERN "*.ny"
)
install(DIRECTORY "${NYTRIX_ROOT}/etc/assets" DESTINATION ${CMAKE_INSTALL_DATADIR}/nytrix/etc)
install(DIRECTORY "${NYTRIX_ROOT}/src" DESTINATION ${CMAKE_INSTALL_DATADIR}/nytrix
  FILES_MATCHING PATTERN "*.h" PATTERN "*.c"
)
install(DIRECTORY "${CMAKE_BINARY_DIR}/docs" DESTINATION ${CMAKE_INSTALL_DATADIR}/nytrix OPTIONAL)
if (UNIX)
  install(DIRECTORY "${CMAKE_BINARY_DIR}/man" DESTINATION ${CMAKE_INSTALL_MANDIR} OPTIONAL)
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
    set(NY_BOLT_WORKLOAD "etc/tests/bench" CACHE STRING "Path to benchmark workload for profiling")

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
          use core.math.nt *
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
