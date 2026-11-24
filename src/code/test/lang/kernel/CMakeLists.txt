# Kernel acceptance tests: build Linux with NYC and boot it in QEMU.
#
#   nyc-kernel-boot         builds the tree named by -DNYC_LINUX_SRC=/path.
#   nyc-kernel-boot-latest  clones the newest upstream release first
#                           (-DNYC_KERNEL_CLONE=ON); see run.cmake.
#
# Both take about fifteen minutes and are serialized under the "kernel" label,
# so run them with `ctest -L kernel`.

set(NYC_LINUX_SRC "" CACHE PATH
    "Linux source tree for the NYC kernel boot test (empty disables it)")
option(NYC_KERNEL_CLONE
       "Register nyc-kernel-boot-latest, which clones upstream Linux" OFF)
set(NYC_LINUX_REPO "https://github.com/torvalds/linux.git" CACHE STRING
    "Git repository the latest-kernel test clones")
set(NYC_LINUX_REF "latest" CACHE STRING
    "Tag to clone: 'latest' picks the newest non-rc release tag")

if(NYC_LINUX_SRC AND EXISTS "${NYC_LINUX_SRC}/Makefile")
  add_test(
    NAME nyc-kernel-boot
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/kernel-boot-smoke.sh"
      "${NYC_LINUX_SRC}" "${CMAKE_BINARY_DIR}/nyc-kernel-boot"
      "$<TARGET_FILE:nyc>"
  )
  set_tests_properties(nyc-kernel-boot PROPERTIES TIMEOUT 3600
    RUN_SERIAL TRUE LABELS "kernel")
endif()

if(NYC_KERNEL_CLONE)
  add_test(
    NAME nyc-kernel-boot-latest
    COMMAND "${CMAKE_COMMAND}"
      "-DNYC=$<TARGET_FILE:nyc>"
      "-DSCRIPT_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DWORK_DIR=${CMAKE_BINARY_DIR}/nyc-kernel-latest"
      "-DLINUX_REPO=${NYC_LINUX_REPO}"
      "-DLINUX_REF=${NYC_LINUX_REF}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run.cmake"
  )
  set_tests_properties(nyc-kernel-boot-latest PROPERTIES TIMEOUT 7200
    RUN_SERIAL TRUE LABELS "kernel;network")
endif()
