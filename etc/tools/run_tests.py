#!/usr/bin/env python3
"""
Nytrix Test Orchestrator
"""
import os
import sys
import time
import subprocess
import glob
import re
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed

RESET = "\033[0m"
GRAY = "\033[90m"
RED = "\033[31m"
GREEN = "\033[32m"
CYAN = "\033[36m"
YELLOW = "\033[33m"
BOLD = "\033[1m"

def parse_time_ns(output):
    match = re.search(r"Time \(ns\):\s*(\d+)", output)
    return int(match.group(1)) if match else None

def shorten_path(path):
    display = path
    if display.startswith("test/"):
        display = display[5:]
    if len(display) > 50:
        parts = display.split('/')
        if len(parts) > 2:
            display = f"{parts[0]}/.../{parts[-1]}"
    return display

def run_process(cmd, input_str=None):
    start = time.time()
    try:
        result = subprocess.run(
            cmd,
            input=input_str,
            capture_output=True,
            text=True,
            timeout=30
        )
        duration = time.time() - start
        return {
            "passed": result.returncode == 0,
            "duration": duration,
            "output": result.stdout + result.stderr
            # "time_ns": parse_time_ns(...) # optimization: skip regex if not needed
        }
    except Exception as e:
        return {"passed": False, "duration": 0, "output": str(e)}

def run_test_pair(path, bin_path, do_repl):
    # 1. Run AOT
    aot_res = run_process([bin_path, "-std", path])
    # 2. Run REPL
    repl_res = None
    if do_repl:
        try:
            with open(path, "r") as f:
                script = f.read()
            repl_res = run_process([bin_path, "-i"], input_str=script)
        except Exception as e:
            repl_res = {"passed": False, "duration": 0, "output": str(e)}
    return path, aot_res, repl_res

def print_result_line(path, aot_res, repl_res, idx, total):
    percent = f"{int((idx / total) * 100):>3}%"
    display = shorten_path(path)
    
    aot_mark = f"{GREEN}✓{RESET}" if aot_res['passed'] else f"{RED}✗{RESET}"
    
    if repl_res:
        # --- Dual Mode ---
        repl_mark = f"{GREEN}✓{RESET}" if repl_res['passed'] else f"{RED}✗{RESET}"
        status = f"[{aot_mark}/{repl_mark}]"
        
        # Timings: "AAAA:RRRRms" (No space before ms)
        aot_ms = int(aot_res['duration'] * 1000)
        repl_ms = int(repl_res['duration'] * 1000)
        times = f"{GRAY}{aot_ms:>4}:{repl_ms:>4}ms{RESET}"
        
        # Delta: "+123ms" (No trailing spaces)
        diff_ms = int((repl_res['duration'] - aot_res['duration']) * 1000)
        if abs(diff_ms) < 2:
            delta = f"{GRAY}   ·   {RESET}"
        else:
            color = GREEN if diff_ms <= 0 else RED
            sign = '+' if diff_ms > 0 else ' '
            delta = f"{color}{sign}{abs(diff_ms):>4}ms{RESET}"
            
    else: 
        # --- Single Mode ---
        status = f"[{aot_mark}/-]"
        aot_ms = int(aot_res['duration'] * 1000)
        times = f"{GRAY}{aot_ms:>4}ms     {RESET}" 
        delta = f"{YELLOW}  SKIP {RESET}"

    # Layout: PERCENT | STATUS | TIMINGS | DELTA | PATH
    # Exactly one space between columns
    print(f"{GRAY}{percent}{RESET} {status} {times} {delta} {display}")
    
    aot_ok = aot_res['passed']
    repl_ok = repl_res['passed'] if repl_res else True
    return aot_ok and repl_ok

def run_suite(name, tests, bin_path, jobs, do_repl):
    # Header
    width = 65
    name_fmt = f" [ {CYAN}{name}{RESET}{GRAY} ] "
    dash_count = (width - len(name) - 6) // 2
    dashes = "-" * dash_count
    print(f"{GRAY}{dashes}{name_fmt}{dashes}{RESET}")
    
    results_data = []
    passed_count = 0
    total = len(tests)
    
    futures = []
    
    if jobs == 1:
        for idx, path in enumerate(tests, 1):
            _, aot, repl = run_test_pair(path, bin_path, do_repl)
            if print_result_line(path, aot, repl, idx, total):
                passed_count += 1
            results_data.append((path, aot['duration']))
    else:
        with ProcessPoolExecutor(max_workers=jobs) as ex:
            for path in tests:
                futures.append(ex.submit(run_test_pair, path, bin_path, do_repl))
            completed = 0
            for fut in as_completed(futures):
                completed += 1
                path, aot, repl = fut.result()
                if print_result_line(path, aot, repl, completed, total):
                    passed_count += 1
                results_data.append((path, aot['duration']))
                
    return passed_count, total, results_data

def main():
    parser = argparse.ArgumentParser(description="Nytrix Unified Test Runner")
    parser.add_argument("--bin", default="build/ny", help="Binary path")
    parser.add_argument("--jobs", type=int, default=0, help="Parallel jobs")
    parser.add_argument("--pattern", help="Test filter regex")
    args = parser.parse_args()
    
    bin_path = os.path.abspath(args.bin)
    debug_bin = bin_path + "_debug" if not bin_path.endswith("_debug") else bin_path
    
    if not os.path.exists(bin_path):
        if os.path.exists("build/ny"): bin_path = "build/ny"
        elif os.path.exists("ny"): bin_path = "ny"
        else:
            print(f"{RED}Binary {args.bin} not found{RESET}")
            sys.exit(1)

    jobs = args.jobs or int(os.getenv("JOBS") or os.getenv("NY_JOBS") or os.cpu_count() or 1)
    
    suites = [
        ("Bench", "test/bench/*.ny", bin_path, True),
        ("Runtime", "test/runtime/**/*.ny", debug_bin if os.path.exists(debug_bin) else bin_path, True),
        ("Std", "test/std/**/*.ny", debug_bin if os.path.exists(debug_bin) else bin_path, True),
    ]

    total_passed = 0
    total_count = 0
    all_timings = []
    start_time = time.time()

    for name, pattern, binary, do_repl in suites:
        files = glob.glob(pattern, recursive=True)
        if args.pattern:
            regex = re.compile(args.pattern)
            files = [t for t in files if regex.search(t)]
        files = sorted(files)
        if not files: continue
        passed, count, timings = run_suite(name, files, binary, jobs, do_repl)
        total_passed += passed
        total_count += count
        all_timings.extend(timings)
    
    total_dur = int((time.time() - start_time) * 1000)
    print(f"{GRAY}-----------------------------------------------------------------{RESET}")
    print(f"{BOLD}Slowest Tests:{RESET}")
    all_timings.sort(key=lambda x: x[1], reverse=True)
    for path, dur in all_timings[:5]:
        print(f" {RED}{dur*1000:>4.0f}ms{RESET} {shorten_path(path)}")
    print(f"{GRAY}-----------------------------------------------------------------{RESET}")
    
    if total_passed == total_count:
        color = GREEN
    else:
        color = RED

    print(f"Total: {total_count} tests | {color}{total_passed} passed{RESET} | {RED}{total_count - total_passed} failed{RESET}")
    print(f"Time:  {total_dur} ms ({jobs} threads)")
    
    if total_passed != total_count:
        sys.exit(1)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print(f"\n{RED}Interrupted{RESET}")
        sys.exit(1)