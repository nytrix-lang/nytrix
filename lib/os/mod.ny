;; Keywords: os filesystem path process subprocess io time thread async parallel atomic clipboard gpu opencl hardware platform
;; Operating-system facade: paths, files, processes, time, threads, async tasks, hardware status, acceleration, and clipboard.
module std.os(pid, ppid, env, environ, getcwd, uid, gid, file_read, file_write, file_exists, file_append, file_remove, os, arch, argv, args, path_sep, path_has_sep, path_is_abs, path_join, path_normalize, path_basename, path_dirname, path_extname, path_splitext, path_resolve_repo_asset, temp_dir, home_dir, config_dir, data_dir, cache_dir, is_file, is_dir, list_dir, walk, time, now, unix, now_ms, sleep, msleep, ticks, monotonic_ns, Instant, instant, since_ns, since_ms, Timer, timer, timer_start, elapsed_ns, elapsed_ms, elapsed_sec, format, format_time, run, popen, waitpid, spawn, send, sendline, recv, recv_line, recv_all, shutdown_send, close, run_capture, check_output, output, check_lines, shell, shell_lines, gpu_mode, gpu_backend, gpu_offload, gpu_min_work, gpu_async, gpu_fast_math, gpu_available, gpu_should_offload, gpu_offload_status, accel_target, accel_targets, accel_target_available, accel_target_triple, accel_binary_kind, accel_binary_ext, accel_backend, accel_target_status, accel_compile_plan, accel_emit_plan, accel_emit_command, parallel_mode, parallel_threads, parallel_min_work, parallel_should_threads, parallel_status, scheduler_policy, scheduler_status, work_stealing_enabled, work_stealing_plan, work_queue, work_queue_push, work_queue_pop, work_queue_steal, thread_spawn, thread_spawn_call, thread_launch, thread_launch_call, thread_join, mutex_new, mutex_lock, mutex_unlock, mutex_free, hardware_threads, atomic_i64, atomic_free, atomic_load, atomic_store, atomic_add, atomic_sub, atomic_exchange, atomic_compare_exchange, thread_budget, future, async, await, await_all, detach, future_wait, async_yield_now, async_sleep_ms, async_run, async_backend, async_state, parallel_map, parallel_map_indexed, parallel_each, chunk_ranges, opencl_available, opencl_toolchain_available, opencl_async, opencl_fast_math, opencl_should_offload, opencl_status, opencl_device_policy, opencl_compile_plan, opencl_kernel_plan, opencl_cpu_fallback_plan, opencl_dispatch_plan, opencl_work_groups, OS, ARCH, IS_LINUX, IS_MACOS, IS_WINDOWS, IS_X86_64, IS_AARCH64, IS_ARM, GPU_MODE, GPU_BACKEND, GPU_OFFLOAD, GPU_MIN_WORK, GPU_ASYNC, GPU_FAST_MATH, GPU_AVAILABLE, ACCEL_TARGET, ACCEL_OBJECT, PARALLEL_MODE, PARALLEL_THREADS, PARALLEL_MIN_WORK, SCHEDULER_POLICY, HARDWARE_THREADS, OPENCL_AVAILABLE, OPENCL_TOOLCHAIN_AVAILABLE, hardware_status, set_clipboard_text, get_clipboard_text, exit, fetch)
use std.core
use std.core.common as common
use std.core.error
use std.core.str
use std.os.sys
use std.core.io
use std.os.path as ospath
use std.os.net (requests_get_parsed)
use std.core.dict_mod
use std.os.prim as osprim
use std.os.gpu as osg
use std.os.parallel as ospar
use std.os.async as osasync
use std.os.thread as osthread
use std.os.atomic as osatomic
use std.os.args as osargs
use std.os.time as ostime
use std.os.process as osproc
use std.os.io as osio
use std.os.subprocess as subprocess

fn _clipboard_tool(): str {
   #linux {
      def wd = env("WAYLAND_DISPLAY")
      if(is_str(wd) && wd.len > 0){ if(file_exists("/usr/bin/wl-copy") || file_exists("/usr/local/bin/wl-copy")){ return "wl" } }
      def xd = env("DISPLAY")
      if(is_str(xd) && xd.len > 0){
         if(file_exists("/usr/bin/xclip") || file_exists("/usr/local/bin/xclip")){ return "xclip" }
         if(file_exists("/usr/bin/xsel") || file_exists("/usr/local/bin/xsel")){ return "xsel" }
      }
   } #endif
   "none"
}

fn _clipboard_env_prefix(): str {
   mut p = ""
   def wd = env("WAYLAND_DISPLAY")
   if(is_str(wd) && wd.len > 0){ p = p + "WAYLAND_DISPLAY=" + wd + " " }
   def xd = env("DISPLAY")
   if(is_str(xd) && xd.len > 0){ p = p + "DISPLAY=" + xd + " " }
   def xa = env("XAUTHORITY")
   if(is_str(xa) && xa.len > 0){ p = p + "XAUTHORITY=" + xa + " " }
   p
}

fn _clipboard_tmp_file(str: tag): str {
   #windows {
      return temp_dir() + "\\ny_cb_" + tag + "_" + to_str(pid()) + ".txt"
   } #else {
      return temp_dir() + "/ny_cb_" + tag + "_" + to_str(pid()) + ".txt"
   } #endif
}

fn set_clipboard_text(any: text): bool {
   "Sets the system clipboard text."
   def tmp = _clipboard_tmp_file("w")
   def qtmp = "\"" + tmp + "\""
   match file_write(tmp, text){
      ok(ignoredok) -> { ignoredok }
      err(ignorederr) -> { ignorederr  return false }
   }
   defer {
      match file_remove(tmp){
         ok(ignoredok) -> { ignoredok }
         err(ignorederr) -> { ignorederr }
      }
   }
   #linux {
      def tool = _clipboard_tool()
      def pfx = _clipboard_env_prefix()
      if(tool == "wl"){ subprocess.shell(pfx + "wl-copy < " + qtmp + " 2>/dev/null", false, false) return true }
      if(tool == "xclip"){
         subprocess.shell(pfx + "xclip -selection clipboard -i " + qtmp + " 2>/dev/null", false, false)
         subprocess.shell(pfx + "xclip -selection primary -i " + qtmp + " 2>/dev/null", false, false)
         return true
      }
      if(tool == "xsel"){ subprocess.shell(pfx + "xsel --clipboard --input < " + qtmp + " 2>/dev/null", false, false) return true }
   } #elif macos {
      subprocess.shell("pbcopy < " + qtmp, false, false)
      return true
   } #elif windows {
      subprocess.shell("clip < " + qtmp, false, false)
      return true
   } #else {
      return false
   } #endif
}

fn get_clipboard_text(): str {
   "Retrieves text from the system clipboard."
   mut res = ""
   def tmp = _clipboard_tmp_file("r")
   def qtmp = "\"" + tmp + "\""
   defer {
      if(file_exists(tmp)){
         match file_remove(tmp){
            ok(ignoredok) -> { ignoredok }
            err(ignorederr) -> { ignorederr }
         }
      }
   }
   #linux {
      def tool = _clipboard_tool()
      def pfx = _clipboard_env_prefix()
      if(tool == "wl"){ subprocess.shell(pfx + "wl-paste > " + qtmp + " 2>/dev/null", false, false) }
      elif(tool == "xclip"){ subprocess.shell(pfx + "xclip -o -selection clipboard > " + qtmp + " 2>/dev/null", false, false) }
      elif(tool == "xsel"){ subprocess.shell(pfx + "xsel --clipboard --output > " + qtmp + " 2>/dev/null", false, false) }
   } #elif macos {
      subprocess.shell("pbpaste > " + qtmp, false, false)
   } #elif windows {
      subprocess.shell("powershell -command \"Get-Clipboard\" > " + qtmp, false, false)
   } #endif
   if(file_exists(tmp)){
      def rd = file_read(tmp)
      if(is_ok(rd)){
         res = unwrap(rd)
         def n = res.len
         if(n >= 2 && load8(res, n - 2) == 13 && load8(res, n - 1) == 10){ res = str_slice(res, 0, n - 2) }
         elif(n >= 1 && load8(res, n - 1) == 10){ res = str_slice(res, 0, n - 1) }
      }
   }
   res
}

fn fetch(any: url): any {
   "Downloads content from `url` over HTTP/HTTPS and returns the response body.
   HTTPS and redirects use the libcurl-backed client when available.
   On failure returns 0(so runtime tests can skip cleanly)."
   if(!is_str(url) || url.len == 0){ return 0 }
   def r = requests_get_parsed(url)
   if(r == nil){ return 0 }
   if(r.get("ok", false)){ return r.get("body", "") }
   0
}

fn pid(): int { osprim.pid() }

fn ppid(): int { osprim.ppid() }

fn env(str: key): any { osprim.env(key) }

fn environ(): list { osprim.environ() }

fn os(): str { osprim.os() }

fn arch(): str { osprim.arch() }

fn argv(int: i): any { osargs.argv(i) }

fn args(): list { osargs.args() }

fn path_sep(): str { ospath.sep() }

fn path_has_sep(str: p): bool { ospath.has_sep(p) }

fn path_is_abs(str: p): bool { ospath.is_abs(p) }

fn path_join(str: a, str: b): str { ospath.join(a, b) }

fn path_normalize(str: p): str { ospath.normalize(p) }

fn path_basename(str: p): str { ospath.basename(p) }

fn path_dirname(str: p): str { ospath.dirname(p) }

fn path_extname(str: p): str { ospath.extname(p) }

fn path_splitext(str: p): list { ospath.splitext(p) }

fn path_resolve_repo_asset(str: path): str { ospath.resolve_repo_asset(path) }

fn home_dir(): str { ospath.home_dir() }

fn temp_dir(): str { ospath.temp_dir() }

fn config_dir(): str { ospath.config_dir() }

fn data_dir(): str { ospath.data_dir() }

fn cache_dir(): str { ospath.cache_dir() }

fn is_dir(any: path): bool {
   "Returns true if `path` exists and is a directory."
   if(!is_str(path)){ return false }
   if(eq(path, ".") || eq(path, "..")){ return true }
   __is_dir(path_normalize(path)) == 1
}

fn is_file(any: path): bool {
   "Returns true if `path` exists and is not a directory."
   if(!is_str(path)){ return false }
   def p = path_normalize(path)
   file_exists(p) && !is_dir(p)
}

fn list_dir(any: path): list {
   "Returns directory entry names, excluding `.` and `..`."
   if(!is_str(path)){ return list(0) }
   def h = __dir_open(path_normalize(path))
   if(!h){ return list(0) }
   mut files = list(8)
   while(true){
      def name = __dir_read(h)
      if(!name){ break }
      if(eq(name, ".") || eq(name, "..")){ continue }
      files = files.append(name)
   }
   __dir_close(h)
   files
}

fn walk(any: root, fnptr: cb): int {
   "Recursively visits `root` and calls `cb(path)` for every file and directory."
   if(!is_str(root)){ return 0 }
   mut r = path_normalize(root)
   if(r.len == 0){ r = "." }
   if(!file_exists(r)){ return 0 }
   cb(r)
   if(is_dir(r)){
      def items = list_dir(r)
      mut i = 0
      while(i < items.len){
         walk(path_join(r, items.get(i)), cb)
         i += 1
      }
   }
   0
}

fn time(): f64 { ostime.time() }

fn now(): f64 { ostime.now() }

fn unix(): int { ostime.unix() }

fn now_ms(): int { ostime.now_ms() }

fn sleep(any: s): any { ostime.sleep(s) }

fn msleep(any: ms): any { ostime.msleep(ms) }

fn ticks(): int { ostime.ticks() }

fn monotonic_ns(): int { ostime.monotonic_ns() }

fn Instant(): any { ostime.Instant() }

fn instant(): any { ostime.instant() }

fn since_ns(any: start): int { ostime.since_ns(start) }

fn since_ms(any: start): int { ostime.since_ms(start) }

fn Timer(): any { ostime.Timer() }

fn timer(): any { ostime.timer() }

fn timer_start(any: t): any { ostime.timer_start(t) }

fn elapsed_ns(any: t): int { ostime.elapsed_ns(t) }

fn elapsed_ms(any: t): int { ostime.elapsed_ms(t) }

fn elapsed_sec(any: t): f64 { ostime.elapsed_sec(t) }

fn format(any: ts): str { ostime.format(ts) }

fn format_time(any: ts): str { ostime.format_time(ts) }

fn run(str: path, list: args): int { osproc.run(path, args) }

fn popen(str: path, list: args): any { osproc.popen(path, args) }

fn waitpid(int: pid, int: options): int { osproc.waitpid(pid, options) }

fn spawn(str: path, list: args): any { osio.spawn(path, args) }

fn send(any: p, any: data): any { osio.send(p, data) }

fn sendline(any: p, any: data): any { osio.sendline(p, data) }

fn recv(any: p, int: n=1024): str { osio.recv(p, n) }

fn recv_line(any: p): str { osio.recv_line(p) }

fn recv_all(any: p, int: n=1024): str { osio.recv_all(p, n) }

fn shutdown_send(any: p): any { osio.shutdown_send(p) }

fn close(any: p): any { osio.close(p) }

fn run_capture(any: cmd, any: args=[], any: input=nil, bool: check=true): dict { subprocess.run_capture(cmd, args, input, check) }

fn check_output(any: cmd, any: args=[], bool: text=true, bool: strip=false, any: input=nil): str { subprocess.check_output(cmd, args, text, strip, input) }

fn output(any: cmd, any: args=[], bool: strip=false): str { subprocess.output(cmd, args, strip) }

fn check_lines(any: cmd, any: args=[], bool: keep_empty=false, any: input=nil): list { subprocess.check_lines(cmd, args, keep_empty, input) }

fn shell(str: command, bool: check=true, bool: strip=false): str { subprocess.shell(command, check, strip) }

fn shell_lines(str: command, bool: keep_empty=false): list { subprocess.shell_lines(command, keep_empty) }

fn _facade_env_str_or(str: key, str: fallback): str {
   def v = env(key)
   if(is_str(v)){
      def s = strip(v)
      if(s.len > 0){ return s }
   }
   fallback
}

fn _facade_bool_or(any: v, bool: fallback): bool {
   if(!is_str(v)){ return fallback }
   def s = lower(strip(v))
   if(eq(s, "1") || eq(s, "true") || eq(s, "yes") || eq(s, "on")){ return true }
   if(eq(s, "0") || eq(s, "false") || eq(s, "no") || eq(s, "off")){ return false }
   fallback
}

fn _facade_gpu_mode(str: v): str {
   def s = lower(strip(v))
   if(eq(s, "off") || eq(s, "auto") || eq(s, "opencl")){ return s }
   "auto"
}

fn _facade_gpu_backend(str: v): str {
   mut s = lower(strip(v))
   if(eq(s, "off")){ s = "none" }
   if(eq(s, "none") || eq(s, "auto") || eq(s, "opencl") || eq(s, "cuda") || eq(s, "hip") || eq(s, "metal")){ return s }
   "auto"
}

fn _facade_gpu_offload(str: v): str {
   mut s = lower(strip(v))
   if(eq(s, "true") || eq(s, "yes")){ s = "on" }
   if(eq(s, "false") || eq(s, "no")){ s = "off" }
   if(eq(s, "off") || eq(s, "auto") || eq(s, "on") || eq(s, "force")){ return s }
   "auto"
}

fn _facade_accel_target(str: v): str {
   mut s = lower(strip(v))
   if(eq(s, "off")){ s = "none" }
   if(eq(s, "cuda") || eq(s, "ptx")){
      s = "nvptx"
   } elif(eq(s, "hip") || eq(s, "rocm") || eq(s, "gcn") || eq(s, "rdna")){
      s = "amdgpu"
   } elif(eq(s, "opencl") || eq(s, "vulkan") || eq(s, "spv")){
      s = "spirv"
   } elif(eq(s, "hsa") || eq(s, "hsa_code_object") || eq(s, "hsa-code-object") || eq(s, "rocm_hsa")){
      s = "hsaco"
   }
   if(eq(s, "none") || eq(s, "auto") || eq(s, "nvptx") || eq(s, "amdgpu") || eq(s, "spirv") || eq(s, "hsaco")){ return s }
   "auto"
}

fn _facade_accel_object(str: v): str {
   mut s = lower(strip(v))
   if(eq(s, "obj")){ s = "o" }
   if(eq(s, "cubin")){ s = "ptx" }
   if(eq(s, "none") || eq(s, "auto") || eq(s, "ptx") || eq(s, "o") || eq(s, "spv") || eq(s, "hsaco")){ return s }
   "auto"
}

fn _facade_parallel_mode(str: v): str {
   def s = lower(strip(v))
   if(eq(s, "off") || eq(s, "auto") || eq(s, "threads")){ return s }
   "auto"
}

def OS = os()
def ARCH = arch()
def IS_LINUX = eq(OS, "linux")
def IS_MACOS = eq(OS, "macos")
def IS_WINDOWS = eq(OS, "windows")
def IS_X86_64 = eq(ARCH, "x86_64")
def IS_AARCH64 = (eq(ARCH, "aarch64") || eq(ARCH, "arm64"))
def IS_ARM = eq(ARCH, "arm")

fn gpu_mode(): str { osg.gpu_mode() }

fn gpu_backend(): str { osg.gpu_backend() }

fn gpu_offload(): str { osg.gpu_offload() }

fn gpu_min_work(): int { osg.gpu_min_work() }

fn gpu_async(): bool { osg.gpu_async() }

fn gpu_fast_math(): bool { osg.gpu_fast_math() }

fn gpu_available(): bool { osg.gpu_available() }

fn gpu_should_offload(int: work_items=0): bool { osg.gpu_should_offload(work_items) }

fn gpu_offload_status(int: work_items=0): dict { osg.gpu_offload_status(work_items) }

fn accel_target(): str { osg.accel_target() }

fn accel_targets(): list { osg.accel_targets() }

fn accel_target_available(str: target=""): bool { osg.accel_target_available(target) }

fn accel_target_triple(str: target=""): str { osg.accel_target_triple(target) }

fn accel_binary_kind(str: target=""): str { osg.accel_binary_kind(target) }

fn accel_binary_ext(str: target=""): str { osg.accel_binary_ext(target) }

fn accel_backend(str: target=""): str { osg.accel_backend(target) }

fn accel_target_status(str: target=""): dict { osg.accel_target_status(target) }

fn accel_compile_plan(str: input_path, str: output_path="", str: target=""): dict { osg.accel_compile_plan(input_path, output_path, target) }

fn accel_emit_plan(str: function_name, str: ir_path, str: out_dir="", str: target=""): dict { osg.accel_emit_plan(function_name, ir_path, out_dir, target) }

fn accel_emit_command(str: function_name, str: ir_path, str: out_dir="", str: target=""): list { osg.accel_emit_command(function_name, ir_path, out_dir, target) }

fn opencl_available(): bool { osg.opencl_available() }

fn opencl_toolchain_available(): bool { osg.opencl_toolchain_available() }

fn opencl_async(): bool { osg.opencl_async() }

fn opencl_fast_math(): bool { osg.opencl_fast_math() }

fn opencl_should_offload(int: work_items=0): bool { osg.opencl_should_offload(work_items) }

fn opencl_status(int: work_items=0): dict { osg.opencl_status(work_items) }

fn opencl_device_policy(int: work_items=0): dict { osg.opencl_device_policy(work_items) }

fn opencl_compile_plan(str: input_path, str: output_path=""): dict { osg.opencl_compile_plan(input_path, output_path) }

fn opencl_kernel_plan(str: name, int: global_size, int: local_size=0): dict { osg.opencl_kernel_plan(name, global_size, local_size) }

fn opencl_cpu_fallback_plan(int: work_items=0, int: item_cost=1): dict { osg.opencl_cpu_fallback_plan(work_items, item_cost) }

fn opencl_dispatch_plan(str: name, int: global_size, int: local_size=0): dict { osg.opencl_dispatch_plan(name, global_size, local_size) }

fn opencl_work_groups(int: global_size, int: local_size=0): int { osg.opencl_work_groups(global_size, local_size) }
def GPU_MODE = _facade_gpu_mode(_facade_env_str_or("NYTRIX_GPU_MODE", "auto"))
def GPU_BACKEND = _facade_gpu_backend(_facade_env_str_or("NYTRIX_GPU_BACKEND", "auto"))
def GPU_OFFLOAD = _facade_gpu_offload(_facade_env_str_or("NYTRIX_GPU_OFFLOAD", "auto"))
def GPU_MIN_WORK = common.parse_nonneg_int(env("NYTRIX_GPU_MIN_WORK"))
def GPU_ASYNC = _facade_bool_or(env("NYTRIX_GPU_ASYNC"), true)
def GPU_FAST_MATH = _facade_bool_or(env("NYTRIX_GPU_FAST_MATH"), false)
def GPU_AVAILABLE = gpu_available()
def ACCEL_TARGET = _facade_accel_target(_facade_env_str_or("NYTRIX_ACCEL_TARGET", "auto"))
def ACCEL_OBJECT = _facade_accel_object(_facade_env_str_or("NYTRIX_ACCEL_OBJECT", "auto"))
def OPENCL_AVAILABLE = opencl_available()
def OPENCL_TOOLCHAIN_AVAILABLE = opencl_toolchain_available()

fn parallel_mode(): str { ospar.parallel_mode() }

fn parallel_threads(): int { ospar.parallel_threads() }

fn parallel_min_work(): int { ospar.parallel_min_work() }

fn parallel_should_threads(int: work_items=0): bool { ospar.parallel_should_threads(work_items) }

fn parallel_status(int: work_items=0): dict { ospar.parallel_status(work_items) }

fn scheduler_policy(): str { ospar.scheduler_policy() }

fn scheduler_status(int: work_items=0): dict { ospar.scheduler_status(work_items) }

fn work_stealing_enabled(int: work_items=0): bool { ospar.work_stealing_enabled(work_items) }

fn work_stealing_plan(int: work_items=0, int: max_threads=0): dict { ospar.work_stealing_plan(work_items, max_threads) }

fn work_queue(int: id=0){ ospar.work_queue(id) }

fn work_queue_push(dict: q, any: task){ ospar.work_queue_push(q, task) }

fn work_queue_pop(dict: q){ ospar.work_queue_pop(q) }

fn work_queue_steal(list: queues, int: victim=0){ ospar.work_queue_steal(queues, victim) }

fn hardware_threads(): int { ospar.hardware_threads() }

fn thread_budget(int: work_items=0, int: max_threads=0): int { ospar.thread_budget(work_items, max_threads) }

fn future(fnptr: work, any: arg=0): any { osasync.future(work, arg) }

fn async(fnptr: work, any: arg=0): any { osasync.async(work, arg) }

fn await(any: h): any { osasync.await(h) }

fn await_all(list: handles): list { osasync.await_all(handles) }

fn detach(fnptr: work, any: arg=0): any { osasync.detach(work, arg) }

fn future_wait(any: h): any { osasync.future_wait(h) }

fn async_yield_now(): any { osasync.yield_now() }

fn async_sleep_ms(int: ms): any { osasync.sleep_ms(ms) }

fn async_run(any: h): any { osasync.run(h) }

fn async_backend(): str { osasync.backend() }

fn async_state(any: h): int { osasync.state(h) }

fn parallel_map(list: xs, fnptr: f, int: max_threads=0): list { ospar.parallel_map(xs, f, max_threads) }

fn parallel_map_indexed(list: xs, fnptr: f, int: max_threads=0): list { ospar.parallel_map_indexed(xs, f, max_threads) }

fn parallel_each(list: xs, fnptr: f, int: max_threads=0): int { ospar.parallel_each(xs, f, max_threads) }

fn chunk_ranges(int: count, int: workers): list { ospar.chunk_ranges(count, workers) }
def PARALLEL_MODE = _facade_parallel_mode(_facade_env_str_or("NYTRIX_PARALLEL_MODE", "auto"))
def PARALLEL_THREADS = common.parse_nonneg_int(env("NYTRIX_PARALLEL_THREADS"))
def PARALLEL_MIN_WORK = common.parse_nonneg_int(env("NYTRIX_PARALLEL_MIN_WORK"))
def SCHEDULER_POLICY = ospar.scheduler_policy()
def HARDWARE_THREADS = hardware_threads()

fn hardware_status(int: work_items=0): dict {
   "Returns a cross-platform hardware facade summary for CPU parallelism, GPU, and OpenCL policy."
   {"os": OS, "arch": ARCH, "parallel": parallel_status(work_items), "scheduler": scheduler_status(work_items),
      "hardware_threads": hardware_threads(),
   "thread_budget": thread_budget(work_items), "gpu": gpu_offload_status(work_items), "opencl": opencl_status(work_items)}
}

fn thread_spawn(fnptr: target, any: arg=0): any { osthread.thread_spawn(target, arg) }

fn thread_spawn_call(fnptr: target, any: args=[]): any { osthread.thread_spawn_call(target, args) }

fn thread_launch(fnptr: target, any: arg=0): any { osthread.thread_launch(target, arg) }

fn thread_launch_call(fnptr: target, any: args=[]): any { osthread.thread_launch_call(target, args) }

fn thread_join(any: h): any { osthread.thread_join(h) }

fn mutex_new(): any { osthread.mutex_new() }

fn mutex_lock(any: m): any { osthread.mutex_lock(m) }

fn mutex_unlock(any: m): any { osthread.mutex_unlock(m) }

fn mutex_free(any: m): any { osthread.mutex_free(m) }

fn atomic_i64(any: initial=0): ptr { osatomic.atomic_i64(initial) }

fn atomic_free(ptr: cell): any { osatomic.atomic_free(cell) }

fn atomic_load(ptr: cell, int: offset=0): any { osatomic.atomic_load(cell, offset) }

fn atomic_store(ptr: cell, any: value, int: offset=0): any { osatomic.atomic_store(cell, value, offset) }

fn atomic_add(ptr: cell, any: delta=1, int: offset=0): any { osatomic.atomic_add(cell, delta, offset) }

fn atomic_sub(ptr: cell, any: delta=1, int: offset=0): any { osatomic.atomic_sub(cell, delta, offset) }

fn atomic_exchange(ptr: cell, any: value, int: offset=0): any { osatomic.atomic_exchange(cell, value, offset) }

fn atomic_compare_exchange(ptr: cell, any: expected, any: desired, int: offset=0): bool { osatomic.atomic_compare_exchange(cell, expected, desired, offset) }

fn _is_transient_file_error(any: code): bool {
   #windows {
      return code == -22 || code == -13 || code == -5
   } #else {
      return false
   } #endif
}

fn _open_with_retry(str: path, int: flags, int: mode): Result {
   mut tries = 0
   mut last = err(-1)
   while(tries < 5){
      last = sys_open(path, flags, mode)
      if(is_ok(last)){ return last }
      if(_is_transient_file_error(__unwrap(last))){
         msleep(10)
         tries += 1
         continue
      }
      return last
   }
   last
}

fn _file_write_impl(str: path, any: content, int: flags): Result {
   def p = ospath.normalize(path)
   def open_res = _open_with_retry(p, flags, 420)
   if(is_err(open_res)){ return open_res }
   def fd = unwrap(open_res)
   def n = content.len
   mut off = 0
   while(off < n){
      def w = __write_off(fd, content, n - off, off)
      if(w < 0){
         sys_close_quiet(fd)
         return err(w)
      }
      if(w <= 0){
         sys_close_quiet(fd)
         return err(-5)
      }
      off += w
   }
   sys_close_quiet(fd)
   ok(off)
}

fn getcwd(): str {
   "Returns the current working directory as a string; returns `\"\"` if `getcwd(2)` fails."
   mut buf = malloc(4096)
   if(buf == 0){ return "" }
   mut clen = __getcwd(buf, 4096)
   if(clen <= 0){
      free(buf)
      return ""
   }
   def out = str.cstr_to_str(buf)
   free(buf)
   out
}

@inline
fn uid(): int {
   "Returns the **real user ID** of the calling process via **getuid(2)**."
   __getuid()
}

@inline
fn gid(): int {
   "Returns the **real group ID** of the calling process via **getgid(2)**."
   __getgid()
}

fn file_read(str: path): Result {
   "Reads the whole file at `path`; returns `ok(content_string)` or `err(errno_like_code)`."
   def p = ospath.normalize(path)
   match sys_open(p, 0, 0){ ; O_RDONLY
      ok(fd) -> {
         mut cap = 4096
         mut base = malloc(cap + 16)
         if(base == 0){
            sys_close_quiet(fd)
            return err(-1)
         }
         mut buf = base + 16
         def tmp = malloc(4096)
         if(tmp == 0){
            free(base)
            sys_close_quiet(fd)
            return err(-1)
         }
         mut tlen = 0
         while(true){
            match sys_read(fd, tmp, 4096){
               ok(r) -> {
                  if(r <= 0){ break }
                  if(tlen + r >= cap){
                     while(tlen + r >= cap){ cap = cap * 2 }
                     def nbase = realloc(base, cap + 16)
                     if(nbase == 0){
                        free(tmp, base)
                        sys_close_quiet(fd)
                        return err(-1)
                     }
                     base = nbase
                     buf = base + 16
                  }
                  __copy_mem(ptr_add(buf, tlen), tmp, r)
                  tlen = tlen + r
               }
               err(e) -> {
                  free(tmp, base)
                  sys_close_quiet(fd)
                  return err(e)
               }
            }
         }
         free(tmp)
         store8(buf, 0, tlen)
         sys_close_quiet(fd)
         return ok(init_str(buf, tlen))
      }
      err(e) -> { return err(e) }
   }
}

fn file_write(str: path, any: content): Result {
   "Writes `content` to `path` (truncate/create); returns `ok(bytes_written)` or `err(errno_like_code)`."
   return _file_write_impl(path, content, 577) ; WRONLY|CREAT|TRUNC, 0644
}

fn file_exists(str: path): bool {
   "Returns true when `path` exists(file or directory)."
   def p = ospath.normalize(path)
   mut res = __access(p, 0)
   res == 0
}

fn file_append(str: path, any: content): Result {
   "Appends `content` to `path` (create if missing); returns `ok(bytes_written)` or `err(errno_like_code)`."
   return _file_write_impl(path, content, 1089) ; WRONLY|CREAT|APPEND, 0644
}

fn file_remove(str: path): Result {
   "Removes file `path`; returns `ok(0)` on success or `err(errno_like_code)`."
   def p = ospath.normalize(path)
   mut tries = 0
   mut res = -1
   while(tries < 5){
      res = __unlink(p)
      if(res >= 0){ return ok(0) }
      if(_is_transient_file_error(res)){
         msleep(10)
         tries += 1
         continue
      }
      break
   }
   return err(res)
}

@inline
fn exit(int: code=0): any {
   "Terminates the calling process with the given status code."
   __exit(code)
}
