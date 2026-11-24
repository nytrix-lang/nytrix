# Clone the newest upstream Linux release and run the NYC kernel boot test on it.
#
# Inputs: NYC, SCRIPT_DIR, WORK_DIR, LINUX_REPO, LINUX_REF ("latest" or a tag).
# The clone lives in WORK_DIR/linux and is reused, fetched forward when a newer
# tag appears; the build output goes to WORK_DIR/out and is rebuilt from scratch.

foreach(required NYC SCRIPT_DIR WORK_DIR LINUX_REPO LINUX_REF)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "kernel run.cmake: ${required} is not set")
  endif()
endforeach()

find_program(GIT git REQUIRED)
find_program(BASH bash REQUIRED)

set(ref "${LINUX_REF}")
if(ref STREQUAL "latest")
  execute_process(
    COMMAND "${GIT}" ls-remote --tags --refs --sort=-v:refname "${LINUX_REPO}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE tags ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cannot list tags of ${LINUX_REPO}: ${err}")
  endif()
  string(REPLACE "\n" ";" tag_lines "${tags}")
  set(ref "")
  foreach(line IN LISTS tag_lines)
    if(line MATCHES "refs/tags/(v[0-9]+\\.[0-9]+(\\.[0-9]+)?)$")
      set(ref "${CMAKE_MATCH_1}")
      break()
    endif()
  endforeach()
  if(NOT ref)
    message(FATAL_ERROR "no release tag found in ${LINUX_REPO}")
  endif()
endif()
message(STATUS "upstream Linux release under test: ${ref}")

set(src "${WORK_DIR}/linux")
file(MAKE_DIRECTORY "${WORK_DIR}")
if(EXISTS "${src}/.git")
  execute_process(
    COMMAND "${GIT}" -C "${src}" describe --tags --exact-match HEAD
    OUTPUT_VARIABLE have OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT have STREQUAL ref)
    execute_process(
      COMMAND "${GIT}" -C "${src}" fetch --depth 1 origin "refs/tags/${ref}:refs/tags/${ref}"
      RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "cannot fetch ${ref}")
    endif()
    execute_process(COMMAND "${GIT}" -C "${src}" checkout --force "${ref}"
      RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "cannot check out ${ref}")
    endif()
  endif()
else()
  file(REMOVE_RECURSE "${src}")
  execute_process(
    COMMAND "${GIT}" clone --depth 1 --branch "${ref}" "${LINUX_REPO}" "${src}"
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cannot clone ${LINUX_REPO} at ${ref}")
  endif()
endif()

# Never test a tree with leftover edits or a previous in-tree build.
execute_process(COMMAND "${GIT}" -C "${src}" clean -fdxq)
execute_process(COMMAND "${GIT}" -C "${src}" checkout --force -q "${ref}")

file(REMOVE_RECURSE "${WORK_DIR}/out")
execute_process(
  COMMAND "${BASH}" "${SCRIPT_DIR}/kernel-boot-smoke.sh"
    "${src}" "${WORK_DIR}/out" "${NYC}"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "NYC kernel boot failed on Linux ${ref} (status ${rc})")
endif()
message(STATUS "NYC built and booted Linux ${ref}")
