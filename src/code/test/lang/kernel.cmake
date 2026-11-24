# Kernel acceptance gate: build Linux with NYC and boot it in QEMU.
#
# Included from the top-level CMakeLists it registers
#   nyc-kernel-boot         builds the tree named by -DNYC_LINUX_SRC=/path;
#   nyc-kernel-boot-latest  with -DNYC_KERNEL_CLONE=ON, first clones the newest
#                           non-rc upstream tag (-DNYC_LINUX_REF overrides).
# Run with `ctest -L kernel`; each takes about fifteen minutes.
#
# Run as a script (`cmake -DKERNEL_MODE=boot|latest -DNYC=... -DWORK_DIR=...
# -P kernel.cmake`) it writes the embedded build and boot helpers into
# WORK_DIR/helpers, optionally clones the kernel, and runs the gate.

if(NOT CMAKE_SCRIPT_MODE_FILE)
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
      COMMAND "${CMAKE_COMMAND}" -DKERNEL_MODE=boot
        "-DNYC=$<TARGET_FILE:nyc>" "-DLINUX_SRC=${NYC_LINUX_SRC}"
        "-DWORK_DIR=${CMAKE_BINARY_DIR}/nyc-kernel-boot"
        -P "${CMAKE_CURRENT_LIST_FILE}"
    )
    set_tests_properties(nyc-kernel-boot PROPERTIES TIMEOUT 3600
      RUN_SERIAL TRUE LABELS "kernel")
  endif()
  if(NYC_KERNEL_CLONE)
    add_test(
      NAME nyc-kernel-boot-latest
      COMMAND "${CMAKE_COMMAND}" -DKERNEL_MODE=latest
        "-DNYC=$<TARGET_FILE:nyc>" "-DLINUX_REPO=${NYC_LINUX_REPO}"
        "-DLINUX_REF=${NYC_LINUX_REF}"
        "-DWORK_DIR=${CMAKE_BINARY_DIR}/nyc-kernel-latest"
        -P "${CMAKE_CURRENT_LIST_FILE}"
    )
    set_tests_properties(nyc-kernel-boot-latest PROPERTIES TIMEOUT 7200
      RUN_SERIAL TRUE LABELS "kernel;network")
  endif()
  return()
endif()

# ---------------------------------------------------------------------------
# Script mode.
# ---------------------------------------------------------------------------

foreach(required KERNEL_MODE NYC WORK_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "kernel.cmake: ${required} is not set")
  endif()
endforeach()
find_program(BASH bash REQUIRED)
file(MAKE_DIRECTORY "${WORK_DIR}/helpers")

set(helper_kernel_boot_smoke_sh [==[
#!/bin/bash
# Build Linux with NYC (strict NYC -S + GNU as wrapper), boot the bzImage in QEMU with a
# freestanding initramfs /init, and require NYC_KERNEL_INIT_REACHED on the serial console.
# usage: kernel-boot-smoke.sh LINUX_SRC OUT_DIR NYC_BINARY   (run from the repository root)
set -e
SRC=$(realpath "$1"); OUT=$(realpath -m "$2"); NYC=$(realpath "$3")
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT/build"
cp "$NYC" "$OUT/nyc"
cp "$HERE/nyc-kbuild-strict-cc.sh" "$OUT/cc.sh"; chmod +x "$OUT/cc.sh"
export NYC_BIN="$OUT/nyc" NYC_KERNEL_COMPAT=1 KCFLAGS=-O2
M=(make -C "$SRC" O="$OUT/build" ARCH=x86_64 CC="$OUT/cc.sh" HOSTCC=gcc AS=as LD=ld)
"${M[@]}" x86_64_defconfig >"$OUT/config.log" 2>&1
"${M[@]}" olddefconfig >>"$OUT/config.log" 2>&1
"${M[@]}" V=1 -j"${JOBS:-32}" -k bzImage modules >"$OUT/build.log" 2>&1
python3 "$HERE/kernel-provenance.py" "$OUT/build.log" "$OUT/build" "$OUT/nyc" "$OUT/provenance.json"
cat > "$OUT/init.c" <<'EOC'
static long sc(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
void _start(void){const char m[]="NYC_KERNEL_INIT_REACHED\n";sc(1,1,(long)m,sizeof m-1);sc(169,0xfee1dead,0x28121969,0x4321fedc);for(;;);}
EOC
gcc -static -nostdlib -O1 -fno-pie -no-pie -fno-stack-protector -o "$OUT/init" "$OUT/init.c"
python3 - "$OUT" <<'EOP'
import gzip,stat,sys
o=sys.argv[1]
def ent(name,mode,data=b'',rdev=(0,0),ino=[1]):
    ino[0]+=1; n=name.encode()+b'\0'
    h='070701'+''.join('%08X'%x for x in [ino[0],mode,0,0,1,0,len(data),0,0,rdev[0],rdev[1],len(n),0])
    b=h.encode()+n; b+=b'\0'*((4-len(b)%4)%4); b+=data+b'\0'*((4-len(data)%4)%4); return b
init=open(o+'/init','rb').read()
out=ent('dev',stat.S_IFDIR|0o755)+ent('dev/console',stat.S_IFCHR|0o600,rdev=(5,1))+ent('init',stat.S_IFREG|0o755,init)+ent('TRAILER!!!',0)
open(o+'/initramfs.gz','wb').write(gzip.compress(out))
EOP
cat > "$OUT/init_glibc.c" <<'EOC'
#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>
int main(void){ puts("NYC_KERNEL_INIT_REACHED"); fflush(stdout); sync(); reboot(RB_POWER_OFF); for(;;) pause(); }
EOC
gcc -static -O2 -o "$OUT/init_glibc" "$OUT/init_glibc.c"
python3 - "$OUT" <<'EOP'
import gzip,stat,sys
o=sys.argv[1]
def ent(name,mode,data=b'',rdev=(0,0),ino=[1]):
    ino[0]+=1; n=name.encode()+b'\0'
    h='070701'+''.join('%08X'%x for x in [ino[0],mode,0,0,1,0,len(data),0,0,rdev[0],rdev[1],len(n),0])
    b=h.encode()+n; b+=b'\0'*((4-len(b)%4)%4); b+=data+b'\0'*((4-len(data)%4)%4); return b
init=open(o+'/init_glibc','rb').read()
out=ent('dev',stat.S_IFDIR|0o755)+ent('dev/console',stat.S_IFCHR|0o600,rdev=(5,1))+ent('init',stat.S_IFREG|0o755,init)+ent('TRAILER!!!',0)
open(o+'/initramfs_glibc.gz','wb').write(gzip.compress(out))
EOP
timeout "${BOOT_TIMEOUT:-300}" qemu-system-x86_64 -m 512 -smp 1 -display none -serial file:"$OUT/serial_glibc.log" -no-reboot -nic none \
  -kernel "$OUT/build/arch/x86/boot/bzImage" -initrd "$OUT/initramfs_glibc.gz" \
  -append "console=ttyS0 loglevel=7 panic=-1 rdinit=/init vga=normal nokaslr" -accel "${ACCEL:-tcg}" -cpu "${CPU:-qemu64}" >/dev/null 2>&1 || true
grep -q NYC_KERNEL_INIT_REACHED "$OUT/serial_glibc.log" || { echo "FAIL: static glibc /init marker missing"; tail -n 20 "$OUT/serial_glibc.log"; exit 1; }
echo "PASS: static glibc /init"
timeout "${BOOT_TIMEOUT:-300}" qemu-system-x86_64 -m 512 -smp 1 -display none -serial file:"$OUT/serial.log" -no-reboot -nic none \
  -kernel "$OUT/build/arch/x86/boot/bzImage" -initrd "$OUT/initramfs.gz" \
  -append "console=ttyS0 loglevel=7 panic=-1 rdinit=/init vga=normal nokaslr" -accel "${ACCEL:-tcg}" -cpu "${CPU:-qemu64}" >/dev/null 2>&1 || true
grep -q NYC_KERNEL_INIT_REACHED "$OUT/serial.log" && echo "PASS: kernel reached /init" || { echo "FAIL: /init marker missing"; tail -n 20 "$OUT/serial.log"; exit 1; }
]==])
file(WRITE "${WORK_DIR}/helpers/kernel-boot-smoke.sh" "${helper_kernel_boot_smoke_sh}")

set(helper_kernel_provenance_py [==[
#!/usr/bin/env python3
"""Provenance manifest for an NYC Linux build.

usage: kernel-provenance.py BUILD_LOG OUT_DIR NYC_BINARY [MANIFEST.json]

Pairs every `CC <object>` step of a V=1 Kbuild log that went through the strict
wrapper with the `compiled <source> -> ...nyc.s` line NYC printed for it, and
records the compiler and configuration checksums. Exits non-zero when a C unit
was built by something other than NYC, or an NYC unit has no object.
"""
import hashlib, json, os, re, subprocess, sys

def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def main():
    log, out, nyc = sys.argv[1:4]
    manifest = sys.argv[4] if len(sys.argv) > 4 else os.path.join(out, "provenance.json")
    text = open(log, errors="replace").read()
    cc_objects = re.findall(r"^\s*# CC\s+(\S+\.o)\s*$", text, re.M)
    # parallel output can interleave lines, so match anywhere in a line
    compiled = re.findall(r"compiled (\S+\.c) -> (\S+?)\.nyc\.", text)
    by_object = {}
    for src, stem in compiled:
        by_object[stem + ".o"] = src
    missing = []
    for o in cc_objects:
        if o in by_object:
            continue
        # Interleaved parallel output can cut the "compiled" line; fall back to
        # the object itself: a GCC-built object carries a "GCC:" .comment.
        path = os.path.join(out, o)
        comment = subprocess.run(["readelf", "-p", ".comment", path],
                                 capture_output=True, text=True).stdout
        if os.path.exists(path) and "GCC:" not in comment:
            by_object[o] = "(paired by object: no GCC .comment)"
        else:
            missing.append(o)
    orphan = [o for o in by_object if o not in set(cc_objects)]
    config = os.path.join(out, ".config")
    version = subprocess.run([nyc, "--version"], capture_output=True, text=True).stdout.strip()
    data = {
        "nyc_version": version,
        "nyc_sha256": sha(nyc),
        "config_sha256": sha(config) if os.path.exists(config) else None,
        "build_log_sha256": sha(log),
        "c_units_compiled_by_nyc": len(by_object),
        "cc_steps": len(cc_objects),
        "cc_steps_without_nyc": missing,
        "nyc_units_without_cc_step": orphan,
        "objects": {o: by_object[o] for o in sorted(by_object)},
    }
    with open(manifest, "w") as f:
        json.dump(data, f, indent=1)
    print("units compiled by NYC:", len(by_object), " CC steps:", len(cc_objects),
          " without NYC:", len(missing))
    return 1 if missing else 0

if __name__ == "__main__":
    sys.exit(main())
]==])
file(WRITE "${WORK_DIR}/helpers/kernel-provenance.py" "${helper_kernel_provenance_py}")

set(helper_nyc_kbuild_strict_cc_sh [==[
#!/usr/bin/env bash
set -e
NYC_BIN=${NYC_BIN:?NYC_BIN must name the tested compiler}
is_c=0
compile=0
language=
next_language=0
for argument in "$@"; do
  if [ "$next_language" = 1 ]; then
    language=$argument
    next_language=0
    continue
  fi
  case "$argument" in
    -x) next_language=1 ;;
    -xc) language=c ;;
    -xcpp-output) language=cpp-output ;;
    -xassembler*) language=assembler ;;
    *.c|*.i|-) is_c=1 ;;
    -c) compile=1 ;;
    --version) exec "$NYC_BIN" "$@" ;;
  esac
done
case "$language" in
  c|cpp-output) is_c=1 ;;
  assembler|assembler-with-cpp) is_c=0 ;;
esac
if [ "$is_c" = 0 ]; then
  # MS anonymous-structure syntax is a C-only NYC option. GNU's assembly
  # preprocessor has no C declarations to interpret and rejects this spelling.
  assembler_arguments=()
  for argument in "$@"; do
    [ "$argument" = -fms-anonymous-structs ] ||
      assembler_arguments+=("$argument")
  done
  exec gcc "${assembler_arguments[@]}"
fi
if [ "$compile" = 0 ]; then
  exec "$NYC_BIN" "$@"
fi
arguments=("$@")
output=
assembly_arguments=()
for ((i=0; i<${#arguments[@]}; ++i)); do
  argument=${arguments[$i]}
  if [ "$argument" = -c ]; then
    assembly_arguments+=(-S)
  elif [ "$argument" = -o ]; then
    ((i+=1))
    output=${arguments[$i]}
    assembly_arguments+=(-o "${output%.o}.nyc.s")
  else
    assembly_arguments+=("$argument")
  fi
done
[ -n "$output" ]
assembly="${output%.o}.nyc.s"
"$NYC_BIN" "${assembly_arguments[@]}"
assembler_mode=--64
for argument in "$@"; do
  case "$argument" in -m32|-m16) assembler_mode=--32 ;; esac
done
as "$assembler_mode" -o "$output" "$assembly"
rm -f "$assembly"
]==])
file(WRITE "${WORK_DIR}/helpers/nyc-kbuild-strict-cc.sh" "${helper_nyc_kbuild_strict_cc_sh}")

file(CHMOD "${WORK_DIR}/helpers/kernel-boot-smoke.sh"
     "${WORK_DIR}/helpers/nyc-kbuild-strict-cc.sh"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                 WORLD_READ WORLD_EXECUTE)

set(src "${LINUX_SRC}")
if(KERNEL_MODE STREQUAL "latest")
  foreach(required LINUX_REPO LINUX_REF)
    if(NOT DEFINED ${required})
      message(FATAL_ERROR "kernel.cmake: ${required} is not set")
    endif()
  endforeach()
  find_program(GIT git REQUIRED)
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
  if(EXISTS "${src}/.git")
    execute_process(
      COMMAND "${GIT}" -C "${src}" describe --tags --exact-match HEAD
      OUTPUT_VARIABLE have OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT have STREQUAL ref)
      execute_process(
        COMMAND "${GIT}" -C "${src}" fetch --depth 1 origin
                "refs/tags/${ref}:refs/tags/${ref}"
        RESULT_VARIABLE rc)
      if(NOT rc EQUAL 0)
        message(FATAL_ERROR "cannot fetch ${ref}")
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
  execute_process(COMMAND "${GIT}" -C "${src}" checkout --force -q "${ref}")
  execute_process(COMMAND "${GIT}" -C "${src}" clean -fdxq)
elseif(NOT KERNEL_MODE STREQUAL "boot")
  message(FATAL_ERROR "kernel.cmake: unknown KERNEL_MODE ${KERNEL_MODE}")
endif()

if(NOT EXISTS "${src}/Makefile")
  message(FATAL_ERROR "kernel.cmake: ${src} is not a Linux source tree")
endif()

file(REMOVE_RECURSE "${WORK_DIR}/out")
execute_process(
  COMMAND "${BASH}" "${WORK_DIR}/helpers/kernel-boot-smoke.sh"
          "${src}" "${WORK_DIR}/out" "${NYC}"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "NYC kernel gate failed on ${src} (status ${rc})")
endif()
message(STATUS "NYC built and booted ${src}")
