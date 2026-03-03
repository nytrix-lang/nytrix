#!/usr/bin/env python3
"""
Nytrix Toolchain Management
"""
import sys
sys.dont_write_bytecode = True
import argparse
import os
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
import shutil
import platform
import subprocess
from pathlib import Path

from context import ROOT, host_os
from utils import log, log_ok, warn, err, step, run

ARCH_MAP = {
    "arm": {
        "bin": "arm-linux-gnueabihf-gcc",
        "emu": "qemu-arm",
        "apt": ["gcc-arm-linux-gnueabihf", "g++-arm-linux-gnueabihf", "binutils-arm-linux-gnueabihf", "qemu-user"],
        "pacman": ["qemu-user"],
        "aur": [
            "arm-linux-gnueabihf-binutils",
            "arm-linux-gnueabihf-gcc-stage1",
            "arm-linux-gnueabihf-glibc-headers",
            "arm-linux-gnueabihf-gcc-stage2",
            "arm-linux-gnueabihf-glibc",
            "arm-linux-gnueabihf-gcc"
        ],
        "dnf": ["gcc-arm-linux-gnueabihf", "binutils-arm-linux-gnueabihf", "qemu-user"]
    },
    "aarch64": {
        "bin": "aarch64-linux-gnu-gcc",
        "emu": "qemu-aarch64",
        "apt": ["gcc-aarch64-linux-gnu", "g++-aarch64-linux-gnu", "binutils-aarch64-linux-gnu", "qemu-user"],
        "pacman": ["qemu-user"],
        "aur": [
            "aarch64-linux-gnu-binutils",
            "aarch64-linux-gnu-gcc-stage1",
            "aarch64-linux-gnu-glibc-headers",
            "aarch64-linux-gnu-gcc-stage2",
            "aarch64-linux-gnu-glibc",
            "aarch64-linux-gnu-gcc"
        ],
        "dnf": ["gcc-aarch64-linux-gnu", "binutils-aarch64-linux-gnu", "qemu-user"]
    },
    "riscv64": {
        "bin": "riscv64-linux-gnu-gcc",
        "emu": "qemu-riscv64",
        "apt": ["gcc-riscv64-linux-gnu", "g++-riscv64-linux-gnu", "binutils-riscv64-linux-gnu", "qemu-user"],
        "pacman": ["qemu-user"],
        "aur": [
            "riscv64-linux-gnu-binutils",
            "riscv64-linux-gnu-gcc-stage1",
            "riscv64-linux-gnu-glibc-headers",
            "riscv64-linux-gnu-gcc-stage2",
            "riscv64-linux-gnu-glibc",
            "riscv64-linux-gnu-gcc"
        ],
        "dnf": ["gcc-riscv64-linux-gnu", "binutils-riscv64-linux-gnu", "qemu-user"]
    },
    "windows": {
        "bin": "x86_64-w64-mingw32-gcc",
        "apt": ["mingw-w64"],
        "pacman": ["mingw-w64-gcc"],
        "dnf": ["mingw64-gcc"]
    }
}

def detect_package_manager():
    if shutil.which("apt-get"): return "apt"
    if shutil.which("pacman"): return "pacman"
    if shutil.which("dnf"): return "dnf"
    return None

def install_toolchain(target):
    if target in ("win", "mingw"): target = "windows"

    if target == "all":
        ok = True
        for t in ARCH_MAP:
            ok &= install_toolchain(t)
        return ok

    if target not in ARCH_MAP:
        err(f"Unsupported toolchain target: {target}")
        return False

    info = ARCH_MAP[target]
    if shutil.which(info["bin"]):
        if "emu" not in info or shutil.which(info["emu"]):
            return True

    pm = detect_package_manager()
    if not pm:
        err("Could not detect a supported package manager.")
        return False

    step(f"Installing {target} toolchain via {pm}...")
    try:
        if pm == "apt":
            run(["sudo", "apt-get", "update", "-y"])
            run(["sudo", "apt-get", "install", "-y"] + info["apt"])
        elif pm == "pacman":
            if "pacman" in info:
                try:
                    run(["sudo", "pacman", "-Sy", "--noconfirm"] + info["pacman"])
                except: pass

            if "aur" in info and not shutil.which(info["bin"]):
                for helper in ["yay", "paru"]:
                    if shutil.which(helper):
                        step(f"Using {helper} for step-by-step bootstrap...")
                        for dep in info["aur"]:
                            step(f"Building {dep}...")
                            run([helper, "-S", "--noconfirm", "--needed", dep])
                        break
        elif pm == "dnf":
            run(["sudo", "dnf", "install", "-y"] + info["dnf"])

        if shutil.which(info["bin"]):
            log_ok(f"{target} toolchain installed.")
            return True
        else:
            err(f"Failed to install {target} toolchain.")
            return False

    except Exception as e:
        err(f"Failed to install {target} toolchain: {e}")
        return False

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Install Nytrix cross-compilation toolchains",
    )
    ap.add_argument(
        "target",
        nargs="?",
        default=None,
        help=f"Target: {', '.join(sorted(ARCH_MAP.keys()))}, all, win, mingw",
    )
    args = ap.parse_args(argv)
    if not args.target:
        ap.print_help()
        return 2
    return 0 if install_toolchain(args.target) else 1

if __name__ == "__main__":
    sys.exit(main())
