#!/usr/bin/env python3
"""
Nytrix Cross-Compilation & Toolchain Management
"""
from __future__ import annotations
import sys
sys.dont_write_bytecode = True
import argparse
import os
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
import shutil
import platform
import subprocess
import tempfile
from pathlib import Path

# Internal imports
sys.path.insert(0, str(Path(__file__).resolve().parent))
from context import ROOT, host_os
from utils import log, log_ok, warn, err, step, run

# --- TOOLCHAIN MANAGEMENT (Was toolchain.py) ---

ARCH_MAP = {
    "arm": {
        "bin": "arm-linux-gnueabihf-gcc",
        "emu": "qemu-arm",
        "apt": ["gcc-arm-linux-gnueabihf", "g++-arm-linux-gnueabihf", "binutils-arm-linux-gnueabihf", "qemu-user"],
        "pacman": ["qemu-user"],
        "aur": ["arm-linux-gnueabihf-binutils", "arm-linux-gnueabihf-gcc", "arm-linux-gnueabihf-glibc"],
        "dnf": ["gcc-arm-linux-gnueabihf", "binutils-arm-linux-gnueabihf", "qemu-user"]
    },
    "aarch64": {
        "bin": "aarch64-linux-gnu-gcc",
        "emu": "qemu-aarch64",
        "apt": ["gcc-aarch64-linux-gnu", "g++-aarch64-linux-gnu", "binutils-aarch64-linux-gnu", "qemu-user"],
        "pacman": ["qemu-user"],
        "aur": ["aarch64-linux-gnu-binutils", "aarch64-linux-gnu-gcc", "aarch64-linux-gnu-glibc"],
        "dnf": ["gcc-aarch64-linux-gnu", "binutils-aarch64-linux-gnu", "qemu-user"]
    },
    "riscv64": {
        "bin": "riscv64-linux-gnu-gcc",
        "emu": "qemu-riscv64",
        "apt": ["gcc-riscv64-linux-gnu", "g++-riscv64-linux-gnu", "binutils-riscv64-linux-gnu", "qemu-user"],
        "pacman": ["qemu-user"],
        "aur": ["riscv64-linux-gnu-binutils", "riscv64-linux-gnu-gcc", "riscv64-linux-gnu-glibc"],
        "dnf": ["gcc-riscv64-linux-gnu", "binutils-riscv64-linux-gnu", "qemu-user"]
    },
    "windows": {
        "bin": "x86_64-w64-mingw32-gcc",
        "apt": ["mingw-w64"],
        "pacman": ["mingw-w64-gcc"],
        "dnf": ["mingw64-gcc"]
    }
}

def install_toolchain(target):
    if target in ("win", "mingw"): target = "windows"
    if target == "all": return all(install_toolchain(t) for t in ARCH_MAP)
    if target not in ARCH_MAP:
        err(f"Unsupported toolchain target: {target}")
        return False

    info = ARCH_MAP[target]
    if shutil.which(info["bin"]):
        if "emu" not in info or shutil.which(info["emu"]): return True

    pm = None
    if shutil.which("apt-get"): pm = "apt"
    elif shutil.which("pacman"): pm = "pacman"
    elif shutil.which("dnf"): pm = "dnf"
    
    if not pm:
        err("Could not detect a supported package manager (apt, pacman, dnf).")
        return False

    step(f"Installing {target} toolchain via {pm}...")
    try:
        if pm == "apt":
            run(["sudo", "apt-get", "update", "-y"])
            run(["sudo", "apt-get", "install", "-y"] + info["apt"])
        elif pm == "pacman":
            if "pacman" in info:
                try: run(["sudo", "pacman", "-Sy", "--noconfirm"] + info["pacman"])
                except: pass
            if "aur" in info and not shutil.which(info["bin"]):
                for helper in ["yay", "paru"]:
                    if shutil.which(helper):
                        for dep in info["aur"]: run([helper, "-S", "--noconfirm", "--needed", dep])
                        break
        elif pm == "dnf":
            run(["sudo", "dnf", "install", "-y"] + info["dnf"])
        return bool(shutil.which(info["bin"]))
    except Exception as e:
        err(f"Failed to install {target} toolchain: {e}")
        return False

# --- CROSS COMPILATION (Was cross.py + hardcoded toolchains) ---

TC_LINUX_TMPL = """
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR {proc})
set(TOOLCHAIN_PREFIX {triple})
set(CMAKE_C_COMPILER   ${{TOOLCHAIN_PREFIX}}-gcc)
set(CMAKE_CXX_COMPILER ${{TOOLCHAIN_PREFIX}}-g++)
set(CMAKE_FIND_ROOT_PATH "/usr/${{TOOLCHAIN_PREFIX}}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_C_FLAGS "${{CMAKE_C_FLAGS}} -I/usr/${{TOOLCHAIN_PREFIX}}/include" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${{CMAKE_CXX_FLAGS}} -I/usr/${{TOOLCHAIN_PREFIX}}/include" CACHE STRING "" FORCE)
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)
set(NYTRIX_LLVM_INCLUDE "/usr/${{TOOLCHAIN_PREFIX}}/include" CACHE STRING "" FORCE)
set(NYTRIX_LLVM_CFLAGS "-I/usr/${{TOOLCHAIN_PREFIX}}/include" CACHE STRING "" FORCE)
file(GLOB LLVM_STATIC_LIBS "/usr/${{TOOLCHAIN_PREFIX}}/lib/libLLVM*.a")
set(LLVM_LIBS "")
foreach(lib_path ${{LLVM_STATIC_LIBS}})
    get_filename_component(lib_name ${{lib_path}} NAME_WE)
    string(REPLACE "lib" "" lib_name_no_prefix ${{lib_name}})
    list(APPEND LLVM_LIBS "${{lib_name_no_prefix}}")
endforeach()
list(APPEND LLVM_LIBS "stdc++" "gcc_s" "m" "pthread")
set(LLVM_LIBS "${{LLVM_LIBS}}" CACHE INTERNAL "LLVM libraries" FORCE)
"""

TC_WINDOWS_TMPL = """
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${{TOOLCHAIN_PREFIX}}-gcc)
set(CMAKE_CXX_COMPILER ${{TOOLCHAIN_PREFIX}}-g++)
set(CMAKE_RC_COMPILER  ${{TOOLCHAIN_PREFIX}}-windres)
set(CMAKE_FIND_ROOT_PATH "/usr/${{TOOLCHAIN_PREFIX}}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_C_FLAGS "${{CMAKE_C_FLAGS}} -I/usr/${{TOOLCHAIN_PREFIX}}/include" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${{CMAKE_CXX_FLAGS}} -I/usr/${{TOOLCHAIN_PREFIX}}/include" CACHE STRING "" FORCE)
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_CXX_COMPILER_WORKS TRUE)
set(CMAKE_CROSSCOMPILING_EMULATOR "wine")
set(NYTRIX_LLVM_INCLUDE "/usr/${{TOOLCHAIN_PREFIX}}/include" CACHE STRING "" FORCE)
set(NYTRIX_LLVM_CFLAGS "-I/usr/${{TOOLCHAIN_PREFIX}}/include" CACHE STRING "" FORCE)
file(GLOB LLVM_STATIC_LIBS "/usr/${{TOOLCHAIN_PREFIX}}/lib/libLLVM*.a")
set(NYTRIX_LLVM_LDFLAGS "")
foreach(lib_path ${{LLVM_STATIC_LIBS}})
    get_filename_component(lib_name ${{lib_path}} NAME_WE)
    string(REPLACE "lib" "" lib_name_no_prefix ${{lib_name}})
    list(APPEND NYTRIX_LLVM_LDFLAGS "-l${{lib_name_no_prefix}}")
endforeach()
list(APPEND NYTRIX_LLVM_LDFLAGS "-lstdc++" "-lz" "-lwinpthread" "-lpthread")
set(NYTRIX_LLVM_LDFLAGS "${{NYTRIX_LLVM_LDFLAGS}}" CACHE INTERNAL "LLVM ldflags" FORCE)
"""

TARGET_CONFIGS = {
    "arm": {"proc": "arm", "triple": "arm-linux-gnueabihf", "emu": "qemu-arm", "emu_args": ["-L", "/usr/arm-linux-gnueabihf"]},
    "aarch64": {"proc": "aarch64", "triple": "aarch64-linux-gnu", "emu": "qemu-aarch64", "emu_args": ["-L", "/usr/aarch64-linux-gnu"]},
    "riscv64": {"proc": "riscv64", "triple": "riscv64-linux-gnu", "emu": "qemu-riscv64", "emu_args": ["-L", "/usr/riscv64-linux-gnu"]},
    "windows": {"proc": "x86_64", "triple": "x86_64-w64-mingw32", "ext": ".exe", "emu": "wine"}
}

def cross_compile(target, action="build", jobs=1, unknown=None):
    if target in ("win", "mingw"): target = "windows"
    if target not in TARGET_CONFIGS:
        err(f"Unsupported cross target: {target}")
        return False

    if not install_toolchain(target): return False

    cfg = TARGET_CONFIGS[target]
    step(f"Cross-{action} for {target}...")
    build_dir = ROOT / "build" / f"cross-{target}"
    shutil.rmtree(build_dir, ignore_errors=True)
    build_dir.mkdir(parents=True, exist_ok=True)

    # Generate toolchain file on the fly
    tc_file = build_dir / "toolchain.cmake"
    if target == "windows": tc_content = TC_WINDOWS_TMPL
    else: tc_content = TC_LINUX_TMPL.format(proc=cfg["proc"], triple=cfg["triple"])
    tc_file.write_text(tc_content)

    from detect import detect_host_flags
    tc_cflags, tc_ldflags = detect_host_flags(target)

    cmake_args = [
        "cmake", "-S", str(ROOT), "-B", str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_TOOLCHAIN_FILE={tc_file}",
        f"-DNYTRIX_HOST_CFLAGS={';'.join(tc_cflags)}",
        f"-DNYTRIX_HOST_LDFLAGS={';'.join(tc_ldflags)}",
        "-DNYTRIX_BUILD_COMPILER=ON",
    ]
    if "proc" in cfg: cmake_args.append(f"-DCMAKE_SYSTEM_PROCESSOR={cfg['proc']}")

    try:
        run(cmake_args)
        run(["cmake", "--build", str(build_dir), "-j", str(jobs)])
        if action == "build": return True

        # Test logic
        bin_path = build_dir / ("ny" + cfg.get("ext", ""))
        if not bin_path.exists():
            bin_path = ROOT / "build" / "release" / "ny"
            if not bin_path.exists(): bin_path = ROOT / "build" / "debug" / "ny"
        
        test_cmd = [sys.executable, str(ROOT / "etc" / "tools" / "test.py"),
                    "--bin", str(bin_path), "--std", str(build_dir / "std.ny"), "--triple", cfg["triple"]]
        if cfg.get("emu") and shutil.which(cfg["emu"]):
            test_cmd += ["--emulator", " ".join([cfg["emu"]] + cfg.get("emu_args", []))]
        if unknown: test_cmd.extend(unknown)
        run(test_cmd, env={**os.environ, "NYTRIX_CC": cfg["triple"] + "-gcc", "NYTRIX_HOST_TRIPLE": cfg["triple"]})
        return True
    except Exception as e:
        err(f"Cross-{action} failed: {e}")
        return False

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("target", help="Target arch (arm, aarch64, riscv64, windows)")
    ap.add_argument("action", nargs="?", default="build", choices=["build", "test", "install-toolchain"])
    ap.add_argument("-j", "--jobs", type=int, default=1)
    args, unknown = ap.parse_known_args()
    
    if args.action == "install-toolchain": ok = install_toolchain(args.target)
    else: ok = cross_compile(args.target, args.action, args.jobs, unknown)
    sys.exit(0 if ok else 1)
