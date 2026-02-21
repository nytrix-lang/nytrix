import sys
sys.dont_write_bytecode = True
import os
from context import host_os, is_arm_riscv_machine
from utils import run_capture

def detect_physical_cores(logical=None):
    logical = logical or (os.cpu_count() or 1)
    host = host_os()
    try:
        if host == "linux":
            seen = set()
            phys = core = None
            with open("/proc/cpuinfo", "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    if line.startswith("physical id"):
                        phys = line.split(":", 1)[1].strip()
                    elif line.startswith("core id"):
                        core = line.split(":", 1)[1].strip()
                    elif line.strip() == "":
                        if phys is not None and core is not None:
                            seen.add((phys, core))
                        phys = core = None
            if seen:
                return max(1, len(seen))
            if is_arm_riscv_machine():
                return logical
        elif host == "macos":
            res = run_capture(["sysctl", "-n", "hw.physicalcpu"])
            out = (res.stdout or "").strip()
            if res.returncode == 0 and out.isdigit():
                return max(1, int(out))
    except Exception:
        pass
    return max(1, logical // 2) if logical >= 4 else logical

def smt_factor(logical, physical):
    if physical <= 0:
        return 1.0
    return max(1.0, float(logical)/float(max(1, physical)))

def default_test_jobs_soft_cap(logical):
    if logical >= 64: return max(24, int(logical * 0.75))
    if logical >= 48: return 40
    if logical >= 32: return 32
    if logical >= 16: return 24
    return max(1, logical)

def auto_threads(profile, logical, physical, kind):
    p = (profile or "auto").strip().lower()
    if p == "default" or p == "balanced": p = "auto"
    if p in ("off", "single", "1"): return 1
    if p in ("aggressive", "max"): return logical

    if p in ("conservative", "safe"):
        if kind == "test":
            if host_os() == "windows":
                return max(1, min(4, max(1, physical // 2)))
            return max(1, min(16, max(1, physical // 2)))
        return max(1, min(physical, logical // 2 if logical >= 8 else logical))

    ratio = smt_factor(logical, physical)
    
    if p == "smt":
        if kind == "test":
            if host_os() == "windows":
                return max(1, min(6, max(1, logical // 4 if logical >= 8 else logical // 2)))
            return max(1, min(24, int(logical * 0.6)))
        return max(1, min(logical, int(logical * 0.85)))
        
    if kind == "test":
        if host_os() == "windows":
            base = max(1, physical // 2 if logical >= 12 else physical)
            return max(1, min(10, base))
        if logical <= 4:
            jobs = logical if ratio <= 1.2 else max(1, logical - 1)
        elif ratio >= 1.8:
            reserve = max(1, logical // 8)
            jobs = min(logical - reserve, physical + max(1, physical // 2))
        else:
            reserve = 1 if logical > 2 else 0
            jobs = logical - reserve
        
        jobs = min(jobs, default_test_jobs_soft_cap(logical))
        if jobs < 2 and logical >= 4 and is_arm_riscv_machine():
            jobs = 2
        return max(1, min(logical, jobs))
        
    if ratio >= 1.8:
        jobs = int(physical * 1.5)
    else:
        jobs = int(physical * 1.1)
    return max(1, min(logical, max(physical, jobs)))

def recommended_jobs_for(kind, cap_env_var=""):
    logical = os.cpu_count() or 1
    physical = detect_physical_cores(logical)
    thread_profile = (os.environ.get("NYTRIX_AUTO_THREADS") or "auto").strip().lower()
    jobs = auto_threads(thread_profile, logical, physical, kind)
    if cap_env_var:
        cap = env_int(cap_env_var, 0)
        if cap > 0:
            jobs = min(jobs, cap)
    return max(1, jobs)
