import sys
sys.dont_write_bytecode = True
import os
import shutil
import tempfile
import platform
import urllib.request
import urllib.error
import zipfile
from pathlib import Path

from context import host_os, is_arm_riscv_machine
from utils import (
    run, run_capture, which, warn, err, log, log_ok, 
    cmake_path, env_bool
)
from detect import (
    detect_llvm_version, find_cmake, find_windows_sdk, find_rc,
    find_msvc_dirs, detect_host_triple, windows_sdk_dirs, HOST_CFLAGS, HOST_LDFLAGS
)

def apt_best_llvm_ver():
    res = run_capture("apt-cache search '^llvm-[0-9]+$'", shell=True)
    if res.returncode != 0:
        res = run_capture("apt-cache search llvm-", shell=True)
    vers = set()
    for line in (res.stdout or "").splitlines():
        name = line.split()[0]
        if not name.startswith("llvm-"):
            continue
        try:
            v = int(name.split("-", 1)[1])
            vers.add(v)
        except Exception:
            continue
    return max(vers) if vers else 0

def deps_cmds():
    host = host_os()
    if host == "linux":
        def get_best_ver():
            env_ver = os.environ.get("NYTRIX_LLVM_VERSION", "").strip()
            if env_ver.isdigit():
                return int(env_ver)
            return apt_best_llvm_ver()
        
        info = {}
        try:
            with open('/etc/os-release','r',encoding='utf-8') as f:
                for line in f:
                    line=line.strip()
                    if not line or '=' not in line: continue
                    k,v=line.split('=',1)
                    info[k]=v.strip().strip('\"')
        except Exception:
            pass
            
        distro = info.get('ID','').lower()
        like = info.get('ID_LIKE','').lower()
        
        if distro in ('debian','ubuntu','raspbian','pop','linuxmint') or 'debian' in like:
            v = get_best_ver()
            if v:
                return [
                    'sudo apt update',
                    f"sudo apt install -y build-essential python3 cmake ninja-build git "
                    f"clang-{v} llvm-{v} llvm-{v}-dev llvm-{v}-runtime libreadline-dev"
                ]
            return [
                'sudo apt update',
                'sudo apt install -y build-essential python3 clang cmake ninja-build git llvm-dev libreadline-dev'
            ]
        if distro in ('arch','manjaro') or 'arch' in like:
            return ['sudo pacman -Syu --noconfirm base-devel python clang cmake ninja git llvm readline']
        if distro in ('fedora','rhel','centos','rocky') or 'fedora' in like or 'rhel' in like:
            return ['sudo dnf install -y @development-tools clang llvm-devel cmake ninja-build git readline-devel']
            
    if host == "macos":
        return ["brew install cmake ninja git llvm readline"]
        
    if host == "windows":
        return [
            "winget install -e --id LLVM.LLVM",
            "winget install -e --id Kitware.CMake",
            "winget install -e --id Ninja-build.Ninja",
            "winget install -e --id Git.Git",
        ]
    return []

def ensure_deps():
    missing = []
    host = host_os()
    cmake_path = find_cmake()
    if not cmake_path:
        missing.append("cmake")
    else:
        os.environ["PATH"] = str(Path(cmake_path).parent) + os.pathsep + os.environ.get("PATH", "")
    if not which("git"):
        missing.append("git")
    if not which("ninja") and host_os() != "windows":
        missing.append("ninja")
        
    if host == "windows":
        cand = [
            Path(r"C:\Program Files\LLVM\bin\clang.exe"),
            Path(r"C:\Program Files\LLVM\bin\clang-cl.exe"),
            Path(r"C:\Program Files (x86)\LLVM\bin\clang.exe"),
            Path(r"C:\Program Files (x86)\LLVM\bin\clang-cl.exe"),
        ]
        if not any(p.exists() for p in cand) and not which("clang"):
            missing.append("llvm")
        sdk_root, sdk_ver = find_windows_sdk()
        if not sdk_root or not sdk_ver:
            missing.append("windows-sdk")
        msvc_incs, _ = find_msvc_dirs()
        if not msvc_incs:
            missing.append("msvc")
        if not find_rc():
            missing.append("rc")
            
    if host == "linux":
        info = {}
        try:
            with open('/etc/os-release') as f:
                # Simple parse
                for l in f:
                    if '=' in l:
                        k,v = l.strip().split('=',1)
                        info[k]=v.strip('"')
        except: pass
        
        distro = info.get('ID','').lower()
        if distro in ('debian','ubuntu'):
            want = os.environ.get("NYTRIX_LLVM_VERSION", "").strip()
            ver = int(want) if want.isdigit() else apt_best_llvm_ver()
            if ver:
                if not which(f"clang-{ver}"): missing.append(f"clang-{ver}")
                if not which(f"llvm-config-{ver}"): missing.append(f"llvm-{ver}")

    if not missing:
        return

    auto = os.environ.get("NYTRIX_AUTO_DEPS", "1") == "1"
    if not auto:
        warn("Missing build dependencies: " + ", ".join(missing))
        warn("Set NYTRIX_AUTO_DEPS=1 to auto-install, or install manually.")
        if host == "windows":
             warn("Ensure Visual Studio (Desktop C++) and LLVM are installed.")
        raise SystemExit(1)
        
    cmds = deps_cmds()
    if not cmds:
        raise SystemExit("Missing tools and no installer known for this OS.")
        
    log("DEPS", "installing dependencies")
    if host == "windows":
        for c in cmds:
            if c.startswith("winget"):
                 if "--accept-source-agreements" not in c:
                     c += " --accept-source-agreements --accept-package-agreements --disable-interactivity"
                 res = run_capture(c, shell=True)
                 # Check output for "already installed"
                 if res.returncode != 0:
                     out = (res.stdout or "") + (res.stderr or "")
                     if "already installed" not in out and "No available upgrade" not in out:
                         raise SystemExit(f"Dependency install failed: {c}")
            else:
                 run(c, shell=True)
        return

    for c in cmds:
        run(c, shell=True)

def missing_llvm_c_headers(include_root):
    if not include_root:
        return ["<empty include root>"]
    inc = Path(include_root)
    required = [
        "llvm-c/Core.h", "llvm-c/Analysis.h", "llvm-c/DebugInfo.h",
        "llvm-c/ExecutionEngine.h", "llvm-c/Support.h", "llvm-c/Target.h",
        "llvm-c/TargetMachine.h", "llvm-c/Types.h"
    ]
    missing = [r for r in required if not (inc/r).exists()]
    return missing

def canonical_llvm_c_include_root(include_root):
    if not include_root:
        return ""
    p = Path(include_root)
    cands = []
    for cand in (p, p / "include", p.parent):
        if cand and cand not in cands:
            cands.append(cand)
    if p.name.lower() == "llvm-c":
        cands.insert(0, p.parent)

    for cand in cands:
        core = cand / "llvm-c" / "Core.h"
        if core.exists():
            return str(cand.resolve())
        if cand.name.lower() == "llvm-c" and (cand / "Core.h").exists():
            return str(cand.parent.resolve())

    try:
        for core in p.rglob("Core.h"):
            if core.parent.name.lower() == "llvm-c":
                return str(core.parent.parent.resolve())
    except Exception:
        pass
    return ""

def native_llvm_target_name():
    mach = platform.machine().lower()
    if mach in ("x86_64", "amd64", "i386", "i486", "x86"): return "X86"
    if mach in ("aarch64", "arm64"): return "AArch64"
    if mach.startswith("arm"): return "ARM"
    if "riscv" in mach: return "RISCV"
    return ""

def synthesize_llvm_config_headers(include_root, cc="", llvm_config=""):
    inc = Path(include_root)
    cfg = inc/"llvm"/"Config"
    cfg.mkdir(parents=True, exist_ok=True)

    native = native_llvm_target_name()
    targets = [native] if native else []
    
    def _write_def(path, macro, names):
        path.write_text("".join([f"{macro}({n})\n" for n in names]), encoding="utf-8")

    _write_def(cfg/"Targets.def", "LLVM_TARGET", targets)
    _write_def(cfg/"AsmPrinters.def", "LLVM_ASM_PRINTER", targets)
    _write_def(cfg/"AsmParsers.def", "LLVM_ASM_PARSER", targets)
    _write_def(cfg/"Disassemblers.def", "LLVM_DISASSEMBLER", targets)

    ver = detect_llvm_version(cc, llvm_config) or "21.1.8"
    parts = ver.split(".")
    vmaj = parts[0] if parts else "0"
    vmin = parts[1] if len(parts)>1 else "0"
    vpat = parts[2] if len(parts)>2 else "0"
    triple = detect_host_triple(cc, llvm_config) or ""
    
    native_defs = ""
    if native:
        native_defs = (
            f"#define LLVM_NATIVE_ARCH {native}\n"
            f"#define LLVM_NATIVE_TARGET LLVMInitialize{native}Target\n"
            f"#define LLVM_NATIVE_TARGETINFO LLVMInitialize{native}TargetInfo\n"
            f"#define LLVM_NATIVE_TARGETMC LLVMInitialize{native}TargetMC\n"
            f"#define LLVM_NATIVE_ASMPRINTER LLVMInitialize{native}AsmPrinter\n"
            f"#define LLVM_NATIVE_ASMPARSER LLVMInitialize{native}AsmParser\n"
            f"#define LLVM_NATIVE_DISASSEMBLER LLVMInitialize{native}Disassembler\n"
        )
    
    llvm_config_h = (
        "#ifndef LLVM_CONFIG_H\n#define LLVM_CONFIG_H\n"
        f"#define LLVM_DEFAULT_TARGET_TRIPLE \"{triple}\"\n"
        f"#define LLVM_HOST_TRIPLE \"{triple}\"\n"
        f"#define LLVM_VERSION_MAJOR {vmaj}\n"
        f"#define LLVM_VERSION_MINOR {vmin}\n"
        f"#define LLVM_VERSION_PATCH {vpat}\n"
        f"#define LLVM_VERSION_STRING \"{vmaj}.{vmin}.{vpat}\"\n"
        "#define LLVM_ENABLE_LLVM_C_EXPORT_ANNOTATIONS\n"
        f"{native_defs}#endif\n"
    )
    (cfg/"llvm-config.h").write_text(llvm_config_h, encoding="utf-8")

def find_llvm_c_include_root(llvm_root="", llvm_config=""):
    hdr_env = os.environ.get("NYTRIX_LLVM_HEADERS", "").strip()
    if hdr_env:
        canon = canonical_llvm_c_include_root(hdr_env)
        if canon and not missing_llvm_c_headers(canon):
            return canon
        
    if llvm_config and which(llvm_config):
        try:
            inc = subprocess.check_output([llvm_config, "--includedir"], text=True, stderr=subprocess.DEVNULL).strip()
            if inc:
                canon = canonical_llvm_c_include_root(inc)
                if canon and not missing_llvm_c_headers(canon):
                    return canon
        except: pass
        
    if llvm_root:
        root = Path(llvm_root)
        canon = canonical_llvm_c_include_root(root/"include")
        if canon and not missing_llvm_c_headers(canon):
            return canon
        try:
            for core in root.rglob("Core.h"):
                if core.parent.name == "llvm-c":
                    cand = core.parent.parent
                    canon = canonical_llvm_c_include_root(cand)
                    if canon and not missing_llvm_c_headers(canon):
                        return canon
        except: pass
    return ""

def ensure_llvm_c_headers(build_dir, cc="", llvm_config=""):
    hdr_root = Path(build_dir).resolve()/"third_party"/"llvm"/"headers"/"include"
    
    def _is_healthy(d):
        return not missing_llvm_c_headers(d) and (Path(d)/"llvm"/"Config"/"llvm-config.h").exists()

    if _is_healthy(hdr_root):
        canon = canonical_llvm_c_include_root(hdr_root)
        if canon:
            return canon

    ver = detect_llvm_version(cc, llvm_config) or "21.1.8"
    tag = f"llvmorg-{ver}"
    cache_root = Path(tempfile.gettempdir())/"nytrix-cache"/"llvm-headers"/tag
    cache_inc = cache_root/"include"
    zip_path = cache_root/f"llvm-project-{tag}.zip"
    dl_timeout = int(os.environ.get("NYTRIX_LLVM_DL_TIMEOUT_SEC", "45") or "45")
    dl_retries = int(os.environ.get("NYTRIX_LLVM_DL_RETRIES", "2") or "2")
    dl_timeout = max(5, min(dl_timeout, 600))
    dl_retries = max(1, min(dl_retries, 10))
    zip_override = os.environ.get("NYTRIX_LLVM_HEADERS_ZIP", "").strip()

    if _is_healthy(hdr_root):
        return str(hdr_root.resolve())

    if cache_inc.exists() and not missing_llvm_c_headers(cache_inc):
        log("LLVM", f"staging cached headers from {cache_inc}")
        if hdr_root.exists(): shutil.rmtree(hdr_root, ignore_errors=True)
        shutil.copytree(cache_inc, hdr_root, dirs_exist_ok=True)
        synthesize_llvm_config_headers(hdr_root, cc, llvm_config)
        canon = canonical_llvm_c_include_root(hdr_root)
        if canon:
            return canon

    if os.environ.get("NYTRIX_NO_NET") == "1":
        raise SystemExit("LLVM C headers incomplete. Net disabled.")

    # Download logic
    try: cache_root.mkdir(parents=True, exist_ok=True)
    except: pass
    
    urls = [
         f"https://github.com/llvm/llvm-project/archive/refs/tags/{tag}.zip",
         f"https://codeload.github.com/llvm/llvm-project/zip/refs/tags/{tag}",
    ]
    
    # Optional explicit archive override (network-free path).
    if zip_override:
        zp = Path(zip_override)
        if not zp.exists():
            raise SystemExit(f"NYTRIX_LLVM_HEADERS_ZIP not found: {zp}")
        if not zipfile.is_zipfile(zp):
            raise SystemExit(f"NYTRIX_LLVM_HEADERS_ZIP is not a valid zip: {zp}")
        try:
            cache_root.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(zp, zip_path)
        except Exception as e:
            raise SystemExit(f"Failed to stage NYTRIX_LLVM_HEADERS_ZIP: {e}")

    # Check existing zip
    if zip_path.exists() and zipfile.is_zipfile(zip_path):
        log("LLVM", f"using cached archive {zip_path}")
    else:
        log("LLVM", f"downloading headers {tag} (timeout={dl_timeout}s retries={dl_retries})")
        success = False
        for attempt in range(1, dl_retries + 1):
            for url in urls:
                try:
                    if zip_path.exists():
                        zip_path.unlink()
                    req = urllib.request.Request(
                        url,
                        headers={"User-Agent": "nytrix-build/1.0", "Accept": "application/zip"},
                    )
                    with urllib.request.urlopen(req, timeout=dl_timeout) as r, open(zip_path, "wb") as f:
                        shutil.copyfileobj(r, f)
                    if zipfile.is_zipfile(zip_path):
                        success = True
                        break
                    warn(f"Invalid zip content from {url}")
                except urllib.error.HTTPError as e:
                    warn(f"Download failed {url}: HTTP {e.code}")
                except urllib.error.URLError as e:
                    warn(f"Download failed {url}: {e.reason}")
                except TimeoutError:
                    warn(f"Download timed out {url} after {dl_timeout}s")
                except Exception as e:
                    warn(f"Download failed {url}: {e}")
            if success:
                break
            warn(f"Retrying LLVM header download ({attempt}/{dl_retries})")
        if not success:
            raise SystemExit(
                "Failed to download LLVM headers. "
                "Set NYTRIX_LLVM_HEADERS to an installed LLVM include dir, "
                "or NYTRIX_LLVM_HEADERS_ZIP to a local llvm-project zip."
            )

    # Extract
    try:
        zf = zipfile.ZipFile(zip_path)
    except:
        raise SystemExit("Invalid zip file.")
        
    with zf:
        prefix = zf.namelist()[0].split("/")[0] + "/llvm/include/"
        for name in zf.namelist():
            if not (name.startswith(prefix+"llvm-c/") or name.startswith(prefix+"llvm/Config/")):
                continue
            if name.endswith("/"): continue
            out = hdr_root / name[len(prefix):]
            out.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(name) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst)

    try: shutil.copytree(hdr_root, cache_inc, dirs_exist_ok=True)
    except: pass
    
    synthesize_llvm_config_headers(hdr_root, cc, llvm_config)
    if missing_llvm_c_headers(hdr_root):
        raise SystemExit("LLVM header download process failed (missing headers).")
    canon = canonical_llvm_c_include_root(hdr_root)
    if not canon:
        raise SystemExit("LLVM header staging failed: llvm-c/Core.h not found in staged root.")
    return canon

def ensure_windows_llvm_import_lib(llvm_root, build_dir):
    if host_os() != "windows" or not llvm_root: return ""
    root = Path(llvm_root)
    libdir = root/"lib"
    bindir = root/"bin"
    
    for c in (libdir/"LLVM-C.lib", libdir/"libLLVM-C.lib", bindir/"LLVM-C.lib"):
        if c.exists(): return str(c)
        
    dll = None
    for d in (bindir, libdir):
        if (d/"LLVM-C.dll").exists():
            dll = d/"LLVM-C.dll"
            break
    if not dll: return ""
    
    dlltool = shutil.which("llvm-dlltool") or str(bindir/"llvm-dlltool.exe")
    readobj = shutil.which("llvm-readobj") or str(bindir/"llvm-readobj.exe")
    if not Path(dlltool).exists(): return ""

    out_dir = Path(build_dir)/"third_party"/"llvm"
    out_dir.mkdir(parents=True, exist_ok=True)
    lib_path = out_dir/"LLVM-C.lib"
    
    # Generate lib from dll
    res = run_capture([readobj, "--coff-exports", str(dll)])
    if res.returncode!=0: return ""
    
    exports = []
    for line in res.stdout.splitlines():
        if "Name:" in line:
            name = line.split(":",1)[1].strip()
            if name and name!="<unknown>": exports.append(name)
            
    if not exports: return ""
    (out_dir/"LLVM-C.def").write_text("LIBRARY LLVM-C.dll\nEXPORTS\n" + "\n".join(exports), encoding="utf-8")
    run([dlltool, "-d", str(out_dir/"LLVM-C.def"), "-l", str(lib_path), "-D", "LLVM-C.dll"])
    
    return str(lib_path) if lib_path.exists() else ""

def stage_windows_llvm_runtime(llvm_root, out_dir):
    if host_os() != "windows" or not llvm_root:
        return 0
    root = Path(llvm_root)
    bindir = root/"bin"
    if not bindir.exists():
        return 0

    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)

    copied = 0
    for src in sorted(bindir.glob("*.dll")):
        dst = out/src.name
        try:
            if (
                not dst.exists()
                or dst.stat().st_mtime < src.stat().st_mtime
                or dst.stat().st_size != src.stat().st_size
            ):
                shutil.copy2(src, dst)
                copied += 1
        except OSError:
            continue

    # Some LLVM prebuilt packages expose C API symbols via libLLVM.dll while
    # link steps reference LLVM-C.*. Create a local alias when missing.
    llvm_c = out/"LLVM-C.dll"
    if not llvm_c.exists():
        alias_src = None
        for cand in (out/"libLLVM.dll", out/"LLVM.dll", bindir/"libLLVM.dll", bindir/"LLVM.dll"):
            if cand.exists():
                alias_src = cand
                break
        if alias_src:
            try:
                shutil.copy2(alias_src, llvm_c)
                copied += 1
            except OSError:
                pass

    return copied
