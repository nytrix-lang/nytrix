#!/usr/bin/env python3
"""
Nytrix Cross Compilation
"""
import sys
sys.dont_write_bytecode = True
import argparse
import os
import shutil
from pathlib import Path

from context import ROOT
from utils import log, log_ok, warn, err, step, run
from toolchain import install_toolchain, ARCH_MAP

TARGET_CONFIGS = {
    "arm": {
        "sys": "Linux",
        "proc": "arm",
        "triple": "arm-linux-gnueabihf",
        "cc": "arm-linux-gnueabihf-gcc",
        "cxx": "arm-linux-gnueabihf-g++",
        "emu": "qemu-arm",
        "emu_args": ["-L", "/usr/arm-linux-gnueabihf"]
    },
    "aarch64": {
        "sys": "Linux",
        "proc": "aarch64",
        "triple": "aarch64-linux-gnu",
        "cc": "aarch64-linux-gnu-gcc",
        "cxx": "aarch64-linux-gnu-g++",
        "emu": "qemu-aarch64",
        "emu_args": ["-L", "/usr/aarch64-linux-gnu"]
    },
    "riscv64": {
        "sys": "Linux",
        "proc": "riscv64",
        "triple": "riscv64-linux-gnu",
        "cc": "riscv64-linux-gnu-gcc",
        "cxx": "riscv64-linux-gnu-g++",
        "emu": "qemu-riscv64",
        "emu_args": ["-L", "/usr/riscv64-linux-gnu"]
    },
    "windows": {
        "sys": "Windows",
        "proc": "x86_64",
        "triple": "x86_64-w64-mingw32",
        "cc": "x86_64-w64-mingw32-gcc",
        "cxx": "x86_64-w64-mingw32-g++",
        "ext": ".exe",
        "emu": "wine",
        "llvm_prefix": "/usr/x86_64-w64-mingw32"
    }
}

def cross_compile(target, action="build", jobs=1, unknown=None):
    if target in ("win", "mingw"): target = "windows"
    
    if target not in TARGET_CONFIGS:
        err(f"Unsupported cross-compilation target: {target}")
        return False

    if not install_toolchain(target):
        err(f"Could not ensure toolchain for {target}")
        return False

    cfg = TARGET_CONFIGS[target]
    step(f"Cross-{action} for {target}...")
    build_dir = ROOT / "build" / f"cross-{target}"
    if build_dir.exists():
        shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)
    
    from detect import detect_host_flags
    tc_cflags, tc_ldflags = detect_host_flags(target)

    toolchain_file = ROOT / "etc" / "tools" / f"toolchain-{target}.cmake"
    if not toolchain_file.exists():
        err(f"Toolchain file not found for target {target}: {toolchain_file}")
        return False

    cmake_args = [
        "cmake", "--debug-trycompile", "-S", str(ROOT), "-B", str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}",
        f"-DNYTRIX_HOST_CFLAGS={';'.join(tc_cflags)}",
        f"-DNYTRIX_HOST_LDFLAGS={';'.join(tc_ldflags)}",
        "-DNYTRIX_BUILD_COMPILER=ON",
    ]
    if "proc" in cfg:
        cmake_args.append(f"-DCMAKE_SYSTEM_PROCESSOR={cfg['proc']}")

    try:
        run(cmake_args)
        run(["cmake", "--build", str(build_dir), "-j", str(jobs)])
        
        if action == "build":
            log_ok(f"Cross-build for {target} finished.")
            return True
        
        if action == "test":
            exe = "ny" + cfg.get("ext", "")
            bin_path = build_dir / exe
            
            if bin_path.exists():
                log("TARGET", f"Using target-native compiler: {bin_path}")
            else:
                host_bin = ROOT / "build" / "release" / "ny"
                if not host_bin.exists():
                    host_bin = ROOT / "build" / "debug" / "ny"
                if host_bin.exists():
                    log("CROSS", f"Using host binary as cross-compiler: {host_bin}")
                    bin_path = host_bin
                else:
                    err("No Nytrix compiler binary found. Build host first.")
                    return False

            test_cmd = [sys.executable, str(ROOT / "etc" / "tools" / "test.py"), 
                        "--bin", str(bin_path), 
                        "--std", str(build_dir / "std.ny"),
                        "--triple", cfg["triple"]]
            
            emu = cfg.get("emu")
            if emu:
                if shutil.which(emu):
                    emu_full = [emu] + cfg.get("emu_args", [])
                    test_cmd += ["--emulator", " ".join(emu_full)]
                else:
                    warn(f"Emulator {emu} not found, skipping tests.")
                    return True

            if unknown: test_cmd.extend(unknown)
            test_env = os.environ.copy()
            test_env["NYTRIX_CC"] = cfg["cc"]
            test_env["NYTRIX_HOST_TRIPLE"] = cfg["triple"]
            run(test_cmd, env=test_env)
            log_ok(f"Cross-test for {target} finished.")
            return True

    except Exception as e:
        err(f"Cross-{action} failed: {e}")
        return False

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Cross-build or cross-test Nytrix for a target architecture",
    )
    ap.add_argument(
        "target",
        help=f"Target: {', '.join(sorted(TARGET_CONFIGS.keys()))}, plus aliases: win, mingw",
    )
    ap.add_argument(
        "action",
        nargs="?",
        default="build",
        choices=["build", "test"],
        help="Action to perform (default: build)",
    )
    ap.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=1,
        help="Parallel build jobs (default: 1)",
    )
    args, unknown = ap.parse_known_args(argv)
    ok = cross_compile(args.target, args.action, args.jobs, unknown)
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
