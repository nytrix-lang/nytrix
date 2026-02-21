import sys
sys.dont_write_bytecode = True
import os
import shutil
import platform
import subprocess
import shlex
import tempfile
from pathlib import Path

from context import host_os, is_arm_riscv_machine
from utils import run_capture, which, cmake_path, warn, err

def detect_host_flags():
    arch = platform.machine().lower()
    cflags = []
    ldflags = []
    if host_os() == "linux":
        if arch.startswith("arm") and "64" not in arch:
            cflags += ["-mfloat-abi=hard"]
            ldflags += ["-mfloat-abi=hard"]
            if arch.startswith("armv6"):
                cflags += ["-march=armv6", "-mfpu=vfp"]
                ldflags += ["-mfpu=vfp"]
            elif arch.startswith("armv7") or arch == "armv7l":
                cflags += ["-march=armv7-a", "-mfpu=vfpv3"]
                ldflags += ["-mfpu=vfpv3"]
        elif arch in ("aarch64", "arm64"):
            cflags += ["-march=armv8-a"]
        elif arch in ("x86_64", "amd64"):
            cflags += ["-march=x86-64"]
        elif arch in ("i386", "i486", "i586", "i686"):
            cflags += ["-march=i686"]
    return cflags, ldflags

HOST_CFLAGS, HOST_LDFLAGS = detect_host_flags()

def detect_host_triple(cc, llvm_config=""):
    try:
        out = subprocess.check_output([cc, "-dumpmachine"], text=True).strip()
        if out:
            return out
    except Exception:
        pass
    if llvm_config:
        try:
            out = subprocess.check_output([llvm_config, "--host-target"], text=True).strip()
            if out:
                return out
        except Exception:
            pass
    return ""

def windows_llvm_candidates():
    return [
        r"C:\PROGRA~1\LLVM\bin\clang.exe",
        r"C:\PROGRA~2\LLVM\bin\clang.exe",
        r"C:\PROGRA~1\LLVM\bin\clang-cl.exe",
        r"C:\PROGRA~2\LLVM\bin\clang-cl.exe",
        r"C:\Program Files\LLVM\bin\clang.exe",
        r"C:\Program Files (x86)\LLVM\bin\clang.exe",
        r"C:\Program Files\LLVM\bin\clang-cl.exe",
        r"C:\Program Files (x86)\LLVM\bin\clang-cl.exe",
    ]

def extract_windows_cc_path(raw):
    if not raw:
        return ""
    s = str(raw).strip()
    if not s:
        return ""
    exact = s.strip('"').strip("'")
    if exact and Path(exact).exists():
        return str(Path(exact).resolve())
    try:
        for tok in shlex.split(s, posix=False):
            cand = tok.strip('"').strip("'")
            if cand and cand.lower().endswith(".exe") and Path(cand).exists():
                return str(Path(cand).resolve())
    except ValueError:
        pass
    return ""

def find_cmake():
    path = shutil.which("cmake")
    if path:
        return path
    if host_os() != "windows":
        return ""
    candidates = [
        r"C:\Program Files\CMake\bin\cmake.exe",
        r"C:\Program Files (x86)\CMake\bin\cmake.exe",
        r"C:\Program Files\Kitware\CMake\bin\cmake.exe",
        r"C:\Program Files (x86)\Kitware\CMake\bin\cmake.exe",
    ]
    for c in candidates:
        if Path(c).exists():
            return c
    return ""

def parse_version(text):
    import re
    m = re.search(r"\b(\d+\.\d+\.\d+)\b", text)
    return m.group(1) if m else ""

def detect_llvm_version(cc="", llvm_config=""):
    if llvm_config and which(llvm_config):
        try:
            out = subprocess.check_output([llvm_config, "--version"], text=True, stderr=subprocess.DEVNULL).strip()
            v = parse_version(out)
            if v:
                return v
        except Exception:
            pass
    if cc:
        try:
            out = subprocess.check_output([cc, "--version"], text=True, stderr=subprocess.DEVNULL)
            for line in out.splitlines():
                v = parse_version(line)
                if v:
                    return v
        except Exception:
            pass
    return ""

def find_llvm_config_global(prefer_version=""):
    if prefer_version:
        cand = f"llvm-config-{prefer_version}"
        if shutil.which(cand):
            return cand
        return ""
    names = [f"llvm-config-{v}" for v in range(30, 9, -1)] + ["llvm-config"]
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    return ""

def find_windows_sdk():
    if host_os() != "windows":
        return ("", "")
    
    # Check env vars
    sdk_dir = os.environ.get("WindowsSdkDir") or os.environ.get("WindowsSdkDir10") or ""
    sdk_ver = os.environ.get("WindowsSDKVersion") or os.environ.get("WindowsSdkVersion") or ""
    if sdk_dir: sdk_dir = sdk_dir.rstrip("\\/")
    if sdk_ver: sdk_ver = sdk_ver.strip("\\/")
    if sdk_dir and sdk_ver:
        if (Path(sdk_dir)/"Include"/sdk_ver/"ucrt"/"stdio.h").exists():
            return (sdk_dir, sdk_ver)
            
    # Check registry/disk
    bases = [
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))/"Windows Kits"/"10",
        Path(os.environ.get("ProgramFiles", r"C:\Program Files"))/"Windows Kits"/"10",
    ]
    
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows Kits\Installed Roots") as k:
            root, _ = winreg.QueryValueEx(k, "KitsRoot10")
            if root:
                bases.insert(0, Path(root))
    except Exception:
        pass

    for base in bases:
        inc = base/"Include"
        if not inc.exists():
            continue
        # Find highest version
        vers = [p.name for p in inc.iterdir() if p.is_dir()]
        vers.sort(key=lambda s: [int(x) for x in s.split(".") if x.isdigit()], reverse=True)
        for v in vers:
            if (inc/v/"ucrt"/"stdio.h").exists():
                return (str(base), v)

    return ("", "")

def find_rc():
    path = shutil.which('rc')
    if path: return path
    if host_os() != 'windows': return ''
    
    sdk_root, ver = find_windows_sdk()
    if sdk_root and ver:
        arch = 'x64' if platform.machine().lower() in ('x86_64','amd64') else 'x86'
        cand = Path(sdk_root)/'bin'/ver/arch/'rc.exe'
        if cand.exists(): return str(cand)
    return ''

def windows_sdk_dirs():
    sdk_root, ver = find_windows_sdk()
    if not sdk_root or not ver:
        return [], [], ""
    inc_root = Path(sdk_root)/"Include"/ver
    incs = [inc_root/sub for sub in ("ucrt", "um", "shared", "winrt", "cppwinrt")]
    lib_root = Path(sdk_root)/"Lib"/ver
    arch = "x64" if platform.machine().lower() in ("x86_64", "amd64") else "x86"
    lib_dirs = []
    for sub in ("ucrt", "um"):
        p = lib_root/sub/arch
        if p.exists():
            lib_dirs.append(p)
    return [p for p in incs if p.exists()], lib_dirs, sdk_root

def find_msvc_dirs():
    if host_os() != "windows":
        return [], []
    roots = [
        Path(r"C:\Program Files\Microsoft Visual Studio"),
        Path(r"C:\Program Files (x86)\Microsoft Visual Studio"),
    ]
    msvc_includes = []
    msvc_libs = []
    best = None
    for root in roots:
        if not root.exists(): continue
        for inc in root.rglob(r"VC\Tools\MSVC\*\include"):
            if (inc/"vcruntime.h").exists():
                if not best or inc.as_posix() > best.as_posix():
                    best = inc
    if best:
        msvc_includes.append(best)
        arch = "x64" if platform.machine().lower() in ("x86_64", "amd64") else "x86"
        libdir = best.parent/"lib"/arch
        if libdir.exists():
            msvc_libs.append(libdir)
    return msvc_includes, msvc_libs

def check_windows_toolchain(cc, extra_cflags=None):
    if host_os() != "windows":
        return True
    tmpdir = Path(tempfile.gettempdir())/"nytrix_toolchain_check"
    tmpdir.mkdir(parents=True, exist_ok=True)
    src = tmpdir/"check.c"
    obj = tmpdir/"check.obj"
    src.write_text("#include <stdio.h>\nint main(void){return 0;}\n", encoding="utf-8")
    is_cl = Path(cc).name.lower().startswith("clang-cl") or cc.lower().endswith("clang-cl.exe")
    flags = list(extra_cflags) if extra_cflags else []
    
    # Convert flags for CL if needed
    if is_cl:
        conv = []
        i = 0
        while i < len(flags):
            f = flags[i]
            if f == "-isystem" and i + 1 < len(flags):
                conv.extend(["/I", flags[i + 1]])
                i += 2
            elif f.startswith("-isystem"):
                conv.extend(["/I", f[len("-isystem"):]])
            elif f == "-I" and i + 1 < len(flags):
                conv.extend(["/I", flags[i + 1]])
                i += 2
            elif f.startswith("-I"):
                conv.extend(["/I", f[2:]])
            else:
                conv.append(f)
            i += 1
        flags = conv
    
    cmd = [cc] + flags + (["/c"] if is_cl else ["-c"]) + [str(src), "-o", str(obj)]
    res = run_capture(cmd)
    if res.returncode != 0:
        err("Missing Windows SDK/MSVC headers (stdio.h not found).")
        warn("Install Visual Studio with the 'Desktop development with C++' workload.")
        return False
    return True

def configure_toolchain():
    host = host_os()
    cc_env = os.environ.get("CC")
    cc = cc_env or "clang"
    
    # Linux auto-detect
    if host == "linux" and not os.environ.get("NYTRIX_LLVM_VERSION"):
         # Helper to find best llvm, reused from deps but simple here
         pass 

    llvm_config = os.environ.get("LLVM_CONFIG", "")
    llvm_root = os.environ.get("LLVM_ROOT", "")

    if host == "windows":
        parsed_cc = extract_windows_cc_path(cc_env or "")
        if parsed_cc:
            cc = parsed_cc
    
    if host == "macos":
        # Brew logic
        brew_llvm = ""
        if which("brew"):
             try: brew_llvm = subprocess.check_output(["brew","--prefix","llvm"], text=True).strip()
             except: pass
        if brew_llvm:
             if not llvm_root: llvm_root = brew_llvm
             if not llvm_config:
                  cand = Path(brew_llvm)/"bin"/"llvm-config"
                  if cand.exists(): llvm_config = str(cand)

    if host != "windows" and not llvm_config:
        llvm_config = find_llvm_config_global()

    if host == "windows" and not llvm_root:
        # Default locations
        for r in (r"C:\Program Files\LLVM", r"C:\Program Files (x86)\LLVM"):
            if Path(r).exists():
                llvm_root = r
                break
        if not llvm_root and which(cc):
            p = Path(shutil.which(cc)).parent.parent
            if (p/"bin"/"clang.exe").exists():
                 llvm_root = str(p)

    if host == "windows" and not llvm_config and llvm_root:
        cand = Path(llvm_root)/"bin"/"llvm-config.exe"
        if cand.exists(): llvm_config = str(cand)

    # Cross-pollinate
    if llvm_config and not llvm_root:
        try:
             src = subprocess.check_output([llvm_config, "--prefix"], text=True).strip()
             if src: llvm_root = src
        except: pass

    # Windows PATH fixup
    if host == "windows" and llvm_root:
        bin = Path(llvm_root)/"bin"
        if bin.exists():
            os.environ["PATH"] = str(bin) + os.pathsep + os.environ.get("PATH","")
            
    # Windows CC fixup
    if host == "windows":
         cc_path = shutil.which(cc) if cc else ""
         if cc_path and "windowsapps" in cc_path.lower():
             cc_path = ""
         if cc_path:
             cc = cc_path
         if llvm_root:
             for cand in (
                 str(Path(llvm_root)/"bin"/"clang.exe"),
                 str(Path(llvm_root)/"bin"/"clang-cl.exe"),
             ):
                 if Path(cand).exists():
                     cc = cand
                     break
         if not Path(str(cc)).exists():
             for c in windows_llvm_candidates():
                 if Path(c).exists():
                     cc = c
                     break
    
    return cc, llvm_config, llvm_root
