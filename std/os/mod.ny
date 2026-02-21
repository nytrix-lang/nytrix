;; Keywords: os
;; Os module.

module std.os (
   pid, ppid, env, environ, getcwd, uid, gid, file_read, file_write, file_exists, file_append,
   file_remove, os, arch, gpu_mode, gpu_backend, gpu_offload, gpu_min_work, gpu_async, gpu_fast_math,
   gpu_available, gpu_should_offload, gpu_offload_status,
   accel_target, accel_targets, accel_target_available, accel_target_triple, accel_binary_kind,
   accel_binary_ext, accel_target_status, accel_compile_plan,
   parallel_mode, parallel_threads, parallel_min_work, parallel_should_threads, parallel_status,
   OS, ARCH, IS_LINUX, IS_MACOS, IS_WINDOWS, IS_X86_64, IS_AARCH64, IS_ARM,
   GPU_MODE, GPU_BACKEND, GPU_OFFLOAD, GPU_MIN_WORK, GPU_ASYNC, GPU_FAST_MATH, GPU_AVAILABLE,
   ACCEL_TARGET, ACCEL_OBJECT,
   PARALLEL_MODE, PARALLEL_THREADS, PARALLEL_MIN_WORK
)
use std.core *
use std.str *
use std.os.sys *
use std.str.io *
use std.os.path as ospath

def OS = __os_name()
def ARCH = __arch_name()
def IS_LINUX   = eq(OS, "linux")
def IS_MACOS   = eq(OS, "macos")
def IS_WINDOWS = eq(OS, "windows")
def IS_X86_64  = eq(ARCH, "x86_64")
def IS_AARCH64 = (eq(ARCH, "aarch64") || eq(ARCH, "arm64"))
def IS_ARM     = eq(ARCH, "arm")

fn os(){
   "Returns the name of the operating system (e.g., 'linux', 'macos', 'windows')."
   OS
}

fn arch(){
   "Returns the name of the system architecture (e.g., 'x86_64', 'aarch64')."
   ARCH
}

fn _is_windows(){
   "Internal: returns true on Windows hosts."
   __os_name() == "windows"
}

fn _is_transient_file_error(code){
   "Internal: returns true for transient Windows file operation errors."
   _is_windows() && (code == -22 || code == -13 || code == -5)
}

fn _open_with_retry(path, flags, mode) -> Result {
   "Internal: retries sys_open for transient file errors."
   mut tries = 0
   mut last = err(-1)
   while(tries < 5){
      last = sys_open(path, flags, mode)
      if(is_ok(last)){ return last }
      if(_is_transient_file_error(unwrap_err(last))){
         msleep(10)
         tries += 1
         continue
      }
      return last
   }
   last
}

fn pid(){
   "Returns the **process ID** of the calling process via **getpid(2)**."
   __getpid()
}

fn ppid(){
   "Returns the **parent process ID** of the calling process via **getppid(2)**."
   __getppid()
}

fn env(key){
   "Returns the value of environment variable `key`, or `0` when no matching entry exists."
   def ep = envp()
   if(!ep){ 0 }
   else {
      def key_len = str_len(key)
      mut i = 0
      mut res = 0
      while(load64(ep, i*8)){
         def env_entry = load64(ep, i*8)
         ;; Check if this entry starts with our variable name
         mut matches = 1
         mut j = 0
         while(j < key_len){
            if(load8(env_entry, j) != load8(key, j)){
               matches = 0
               break
            }
            j += 1
         }
         ;; Check for '=' after the name (prevents partial matches)
         if(matches && load8(env_entry, key_len) == 61){
            ;; Found it! Extract the value
            res = cstr_to_str(env_entry, key_len + 1)
            break
         }
         i += 1
      }
      res
   }
}

fn environ(){
   "Returns a list of environment entries in `KEY=VALUE` format."
   def ep = envp()
   if(!ep){ list(8) }
   else {
      def n = envc()
      if(n <= 0){ list(8) }
      else {
         mut xs = list(8)
         mut i = 0
         while(i < n && load64(ep, i*8)){
            def s_raw = load64(ep, i*8)
            xs = append(xs, cstr_to_str(s_raw))
            i += 1
         }
         xs
      }
   }
}

fn _env_str_or(key, fallback){
   "Internal: returns env value as string, or `fallback` when missing."
   def v = env(key)
   if(!is_str(v)){ return fallback }
   def s = strip(v)
   if(str_len(s) == 0){ return fallback }
   s
}

fn _normalize_gpu_mode(v){
   "Internal: normalizes GPU mode to `off|auto|opencl`."
   if(!is_str(v)){ return "auto" }
   def s = lower(strip(v))
   if(eq(s, "off") || eq(s, "auto") || eq(s, "opencl")){ return s }
   "auto"
}

fn _normalize_gpu_backend(v){
   "Internal: normalizes backend to `none|auto|opencl|cuda|hip|metal`."
   if(!is_str(v)){ return "auto" }
   mut s = lower(strip(v))
   if(eq(s, "off")){ s = "none" }
   if(eq(s, "none") || eq(s, "auto") || eq(s, "opencl") || eq(s, "cuda") || eq(s, "hip") || eq(s, "metal")){ return s }
   "auto"
}

fn _normalize_gpu_offload(v){
   "Internal: normalizes offload policy to `off|auto|on|force`."
   if(!is_str(v)){ return "auto" }
   mut s = lower(strip(v))
   if(eq(s, "true") || eq(s, "yes")){ s = "on" }
   if(eq(s, "false") || eq(s, "no")){ s = "off" }
   if(eq(s, "off") || eq(s, "auto") || eq(s, "on") || eq(s, "force")){ return s }
   "auto"
}

fn _normalize_parallel_mode(v){
   "Internal: normalizes parallel mode to `off|auto|threads`."
   if(!is_str(v)){ return "auto" }
   def s = lower(strip(v))
   if(eq(s, "off") || eq(s, "auto") || eq(s, "threads")){ return s }
   "auto"
}

fn _normalize_accel_target(v){
   "Internal: normalizes accelerator target name."
   if(!is_str(v)){ return "auto" }
   mut s = lower(strip(v))
   if(eq(s, "off")){ s = "none" }
   if(eq(s, "cuda") || eq(s, "ptx")){ s = "nvptx" }
   elif(eq(s, "hip") || eq(s, "rocm") || eq(s, "gcn") || eq(s, "rdna")){ s = "amdgpu" }
   elif(eq(s, "opencl") || eq(s, "vulkan") || eq(s, "spv")){ s = "spirv" }
   elif(eq(s, "hsa") || eq(s, "hsa_code_object") || eq(s, "hsa-code-object") || eq(s, "rocm_hsa")){ s = "hsaco" }
   if(eq(s, "none") || eq(s, "auto") || eq(s, "nvptx") || eq(s, "amdgpu") || eq(s, "spirv") || eq(s, "hsaco")){ return s }
   "auto"
}

fn _normalize_accel_object(v){
   "Internal: normalizes requested accelerator object kind."
   if(!is_str(v)){ return "auto" }
   mut s = lower(strip(v))
   if(s == "obj"){ s = "o" }
   if(s == "cubin"){ s = "ptx" }
   if(s == "none" || s == "auto" || s == "ptx" || s == "o" || s == "spv" || s == "hsaco"){ return s }
   "auto"
}

fn _list_has(xs, x){
   "Internal: returns true when list `xs` contains string `x`."
   mut i = 0
   while(i < len(xs)){
      if(eq(get(xs, i, ""), x)){ return true }
      i += 1
   }
   false
}

fn _parse_threads(v){
   "Internal: parses non-negative thread/work values from strings."
   if(!is_str(v)){ return 0 }
   def s = strip(v)
   if(str_len(s) == 0){ return 0 }
   def n = atoi(s)
   if(n < 0){ return 0 }
   n
}

fn _parse_bool_or(v, fallback){
   "Internal: parses bool-ish strings, otherwise returns `fallback`."
   if(!is_str(v)){ return fallback }
   def s = lower(strip(v))
   if(s == "1" || s == "true" || s == "yes" || s == "on"){ return true }
   if(s == "0" || s == "false" || s == "no" || s == "off"){ return false }
   fallback
}

fn _path_exists(path){
   "Internal: returns true when a path exists."
   __access(path, 0) == 0
}

mut _opencl_runtime_loaded = false
mut _opencl_runtime_cache = false
mut _cuda_runtime_loaded = false
mut _cuda_runtime_cache = false
mut _hip_runtime_loaded = false
mut _hip_runtime_cache = false
mut _nvptx_toolchain_loaded = false
mut _nvptx_toolchain_cache = false
mut _amdgpu_toolchain_loaded = false
mut _amdgpu_toolchain_cache = false
mut _spirv_toolchain_loaded = false
mut _spirv_toolchain_cache = false
mut _hsaco_toolchain_loaded = false
mut _hsaco_toolchain_cache = false
mut _auto_accel_target_loaded = false
mut _auto_accel_target_cache = "none"
mut _auto_gpu_backend_loaded = false
mut _auto_gpu_backend_cache = "none"
mut _parallel_threads_eff_loaded = false
mut _parallel_threads_eff_cache = 0
mut _parallel_min_work_eff_loaded = false
mut _parallel_min_work_eff_cache = 0

fn _has_opencl_runtime(){
   "Internal: best-effort OpenCL runtime detection."
   if(_opencl_runtime_loaded){ return _opencl_runtime_cache }
   def force = env("NYTRIX_OPENCL_FORCE")
   if(is_str(force) && str_len(strip(force)) > 0){
      _opencl_runtime_cache = _parse_bool_or(force, false)
      _opencl_runtime_loaded = true
      return _opencl_runtime_cache
   }
   mut out = false
   if(_is_windows()){
      if(_path_exists("C:\\Windows\\System32\\OpenCL.dll")){ out = true }
      elif(_path_exists("C:\\Windows\\SysWOW64\\OpenCL.dll")){ out = true }
      _opencl_runtime_cache = out
      _opencl_runtime_loaded = true
      return out
   }
   if(IS_MACOS){
      out = _path_exists("/System/Library/Frameworks/OpenCL.framework/OpenCL")
      _opencl_runtime_cache = out
      _opencl_runtime_loaded = true
      return out
   }
   if(_path_exists("/etc/OpenCL/vendors")){ out = true }
   elif(_path_exists("/usr/lib/libOpenCL.so")){ out = true }
   elif(_path_exists("/usr/lib64/libOpenCL.so")){ out = true }
   elif(_path_exists("/usr/local/lib/libOpenCL.so")){ out = true }
   elif(_path_exists("/lib/x86_64-linux-gnu/libOpenCL.so.1")){ out = true }
   elif(_path_exists("/usr/lib/x86_64-linux-gnu/libOpenCL.so.1")){ out = true }
   _opencl_runtime_cache = out
   _opencl_runtime_loaded = true
   out
}

fn _has_cuda_runtime(){
   "Internal: best-effort CUDA runtime/device detection."
   if(_cuda_runtime_loaded){ return _cuda_runtime_cache }
   def vis = _env_str_or("CUDA_VISIBLE_DEVICES", "")
   if(vis == "-1"){
      _cuda_runtime_cache = false
      _cuda_runtime_loaded = true
      return false
   }
   mut out = false
   if(_is_windows()){
      if(_path_exists("C:\\Windows\\System32\\nvcuda.dll")){ out = true }
      def cp = _env_str_or("CUDA_PATH", "")
      if(str_len(cp) > 0){ out = true }
      _cuda_runtime_cache = out
      _cuda_runtime_loaded = true
      return out
   }
   if(_path_exists("/dev/nvidiactl")){ out = true }
   elif(_path_exists("/proc/driver/nvidia/version")){ out = true }
   def cp = _env_str_or("CUDA_PATH", "")
   if(str_len(cp) > 0){ out = true }
   _cuda_runtime_cache = out
   _cuda_runtime_loaded = true
   out
}

fn _has_hip_runtime(){
   "Internal: best-effort HIP/ROCm runtime/device detection."
   if(_hip_runtime_loaded){ return _hip_runtime_cache }
   def vis = _env_str_or("HIP_VISIBLE_DEVICES", "")
   if(vis == "-1"){
      _hip_runtime_cache = false
      _hip_runtime_loaded = true
      return false
   }
   mut out = false
   if(_path_exists("/dev/kfd")){ out = true }
   def rp = _env_str_or("ROCM_PATH", "")
   if(str_len(rp) > 0){ out = true }
   def hp = _env_str_or("HIP_PATH", "")
   if(str_len(hp) > 0){ out = true }
   _hip_runtime_cache = out
   _hip_runtime_loaded = true
   out
}

fn _has_nvptx_toolchain(){
   "Internal: best-effort NVIDIA device toolchain detection (clang/nvcc/ptxas)."
   if(_nvptx_toolchain_loaded){ return _nvptx_toolchain_cache }
   mut out = false
   def cp = _env_str_or("CUDA_PATH", "")
   if(str_len(cp) > 0){ out = true }
   elif(_path_exists("/usr/local/cuda/bin/ptxas")){ out = true }
   elif(_path_exists("/usr/local/cuda/bin/nvcc")){ out = true }
   elif(_path_exists("/usr/bin/ptxas")){ out = true }
   elif(_path_exists("/usr/bin/nvcc")){ out = true }
   _nvptx_toolchain_cache = out
   _nvptx_toolchain_loaded = true
   out
}

fn _has_amdgpu_toolchain(){
   "Internal: best-effort AMDGPU backend toolchain detection."
   if(_amdgpu_toolchain_loaded){ return _amdgpu_toolchain_cache }
   mut out = false
   def rp = _env_str_or("ROCM_PATH", "")
   if(str_len(rp) > 0){ out = true }
   def hp = _env_str_or("HIP_PATH", "")
   if(str_len(hp) > 0){ out = true }
   elif(_path_exists("/opt/rocm/bin/amdclang")){ out = true }
   elif(_path_exists("/opt/rocm/bin/clang")){ out = true }
   elif(_path_exists("/opt/rocm/llvm/bin/llc")){ out = true }
   _amdgpu_toolchain_cache = out
   _amdgpu_toolchain_loaded = true
   out
}

fn _has_spirv_toolchain(){
   "Internal: best-effort SPIR-V emission toolchain detection."
   if(_spirv_toolchain_loaded){ return _spirv_toolchain_cache }
   mut out = false
   if(_path_exists("/usr/bin/llvm-spirv")){ out = true }
   elif(_path_exists("/usr/local/bin/llvm-spirv")){ out = true }
   def sdk = _env_str_or("VULKAN_SDK", "")
   if(str_len(sdk) > 0){ out = true }
   elif(_has_opencl_runtime()){ out = true }
   _spirv_toolchain_cache = out
   _spirv_toolchain_loaded = true
   out
}

fn _has_hsaco_toolchain(){
   "Internal: best-effort ROCm HSA code-object toolchain detection."
   if(_hsaco_toolchain_loaded){ return _hsaco_toolchain_cache }
   mut out = false
   if(_has_amdgpu_toolchain()){
      if(_path_exists("/opt/rocm/bin/clang") || _path_exists("/opt/rocm/bin/amdclang")){
         out = true
      }
   }
   _hsaco_toolchain_cache = out
   _hsaco_toolchain_loaded = true
   out
}

fn _backend_to_accel_target(backend){
   "Internal: maps gpu backend to preferred accelerator target."
   def b = _normalize_gpu_backend(backend)
   if(b == "cuda"){ return "nvptx" }
   if(b == "hip"){ return "hsaco" }
   if(b == "opencl"){ return "spirv" }
   "none"
}

fn _pick_auto_accel_target(){
   "Internal: picks preferred accelerator target from runtime/toolchain hints."
   if(_auto_accel_target_loaded){ return _auto_accel_target_cache }
   mut out = "none"
   def bt = _backend_to_accel_target(GPU_BACKEND)
   if(bt != "none" && bt != "auto"){ out = bt }
   elif(_has_cuda_runtime() || _has_nvptx_toolchain()){ out = "nvptx" }
   elif(_has_hip_runtime() || _has_hsaco_toolchain()){ out = "hsaco" }
   elif(_has_opencl_runtime() || _has_spirv_toolchain()){ out = "spirv" }
   elif(_has_amdgpu_toolchain()){ out = "amdgpu" }
   _auto_accel_target_cache = out
   _auto_accel_target_loaded = true
   out
}

fn _resolve_accel_target(target){
   "Internal: resolves configured/explicit accelerator target."
   mut t = ACCEL_TARGET
   if(is_str(target) && str_len(strip(target)) > 0){
      t = _normalize_accel_target(target)
   }
   if(t == "auto"){ t = _pick_auto_accel_target() }
   if(t == "none"){
      def bt = _backend_to_accel_target(GPU_BACKEND)
      if(bt != "none"){ t = bt }
   }
   t
}

fn _target_runtime_available(target){
   "Internal: returns true when runtime/device path appears available for `target`."
   def t = _normalize_accel_target(target)
   if(t == "nvptx"){ return _has_cuda_runtime() }
   if(t == "amdgpu"){ return _has_hip_runtime() || _path_exists("/dev/dri/renderD128") }
   if(t == "spirv"){ return _has_opencl_runtime() }
   if(t == "hsaco"){ return _has_hip_runtime() && _path_exists("/dev/kfd") }
   false
}

fn _target_toolchain_available(target){
   "Internal: returns true when device compilation toolchain appears available."
   def t = _normalize_accel_target(target)
   if(t == "nvptx"){ return _has_nvptx_toolchain() }
   if(t == "amdgpu"){ return _has_amdgpu_toolchain() }
   if(t == "spirv"){ return _has_spirv_toolchain() }
   if(t == "hsaco"){ return _has_hsaco_toolchain() }
   false
}

fn _pick_auto_gpu_backend(){
   "Internal: picks the best available GPU backend."
   if(_auto_gpu_backend_loaded){ return _auto_gpu_backend_cache }
   mut out = "none"
   if(_has_cuda_runtime()){ out = "cuda" }
   elif(_has_hip_runtime()){ out = "hip" }
   elif(_has_opencl_runtime()){ out = "opencl" }
   elif(IS_MACOS){ out = "metal" }
   _auto_gpu_backend_cache = out
   _auto_gpu_backend_loaded = true
   out
}

fn _gpu_backend_available(backend){
   "Internal: returns true when `backend` appears available."
   def b = _normalize_gpu_backend(backend)
   if(b == "none"){ return false }
   if(b == "auto"){ return _pick_auto_gpu_backend() != "none" }
   if(b == "opencl"){ return _has_opencl_runtime() }
   if(b == "cuda"){ return _has_cuda_runtime() }
   if(b == "hip"){ return _has_hip_runtime() }
   if(b == "metal"){ return IS_MACOS }
   false
}

fn _gpu_available_from_env(){
   "Internal: parses `NYTRIX_GPU_AVAILABLE` override as tri-state."
   def raw = _env_str_or("NYTRIX_GPU_AVAILABLE", "")
   if(str_len(raw) == 0){ return -1 }
   if(_parse_bool_or(raw, false)){ return 1 }
   0
}

fn _compute_gpu_available(){
   "Internal: computes effective GPU availability after overrides."
   def ov = _gpu_available_from_env()
   if(ov == 1){ return true }
   if(ov == 0){ return false }
   _gpu_backend_available(GPU_BACKEND)
}

fn _effective_gpu_min_work(){
   "Internal: computes effective min workload for GPU policy."
   if(GPU_MIN_WORK > 0){ return GPU_MIN_WORK }
   if(GPU_BACKEND == "cuda" || GPU_BACKEND == "hip" || GPU_BACKEND == "metal"){ return 2048 }
   4096
}

fn _logical_cpu_guess(){
   "Internal: guesses host logical CPU count from environment hints."
   def n1 = _parse_threads(env("NYTRIX_LOGICAL_CPUS"))
   if(n1 > 0){ return n1 }
   def n2 = _parse_threads(env("NUMBER_OF_PROCESSORS"))
   if(n2 > 0){ return n2 }
   def n3 = _parse_threads(env("NPROC"))
   if(n3 > 0){ return n3 }
   2
}

fn _effective_parallel_threads(){
   "Internal: computes effective thread budget."
   if(_parallel_threads_eff_loaded){ return _parallel_threads_eff_cache }
   mut out = 2
   if(PARALLEL_THREADS > 0){ out = PARALLEL_THREADS }
   else {
      def n = _logical_cpu_guess()
      if(n > 1){ out = n }
   }
   _parallel_threads_eff_cache = out
   _parallel_threads_eff_loaded = true
   out
}

fn _effective_parallel_min_work(){
   "Internal: computes effective min workload for threaded parallel policy."
   if(_parallel_min_work_eff_loaded){ return _parallel_min_work_eff_cache }
   mut out = 65536
   if(PARALLEL_MIN_WORK > 0){ out = PARALLEL_MIN_WORK }
   else {
      def t = _effective_parallel_threads()
      if(t >= 8){ out = 262144 }
      elif(t >= 4){ out = 131072 }
   }
   _parallel_min_work_eff_cache = out
   _parallel_min_work_eff_loaded = true
   out
}

def GPU_MODE = _normalize_gpu_mode(_env_str_or("NYTRIX_GPU_MODE", "auto"))
def GPU_BACKEND = _normalize_gpu_backend(_env_str_or("NYTRIX_GPU_BACKEND", "auto"))
def GPU_OFFLOAD = _normalize_gpu_offload(_env_str_or("NYTRIX_GPU_OFFLOAD", "auto"))
def GPU_MIN_WORK = _parse_threads(env("NYTRIX_GPU_MIN_WORK"))
def GPU_ASYNC = _parse_bool_or(env("NYTRIX_GPU_ASYNC"), true)
def GPU_FAST_MATH = _parse_bool_or(env("NYTRIX_GPU_FAST_MATH"), false)
def GPU_AVAILABLE = _compute_gpu_available()
def ACCEL_TARGET = _normalize_accel_target(_env_str_or("NYTRIX_ACCEL_TARGET", "auto"))
def ACCEL_OBJECT = _normalize_accel_object(_env_str_or("NYTRIX_ACCEL_OBJECT", "auto"))
def PARALLEL_MODE = _normalize_parallel_mode(_env_str_or("NYTRIX_PARALLEL_MODE", "auto"))
def PARALLEL_THREADS = _parse_threads(env("NYTRIX_PARALLEL_THREADS"))
def PARALLEL_MIN_WORK = _parse_threads(env("NYTRIX_PARALLEL_MIN_WORK"))

fn gpu_mode(){
   "Returns the configured GPU mode: `off`, `auto`, or `opencl`.

   Configure via compiler CLI flag `--gpu`."
   GPU_MODE
}

fn gpu_backend(){
   "Returns configured GPU backend: `none`, `auto`, `opencl`, `cuda`, `hip`, or `metal`.

   Configure via compiler CLI flag `--gpu-backend`."
   GPU_BACKEND
}

fn gpu_offload(){
   "Returns GPU offload policy: `off`, `auto`, `on`, or `force`.

   Configure via compiler CLI flag `--gpu-offload`."
   GPU_OFFLOAD
}

fn gpu_min_work(){
   "Returns minimum work threshold before trying GPU offload; `0` means auto/default.

   Configure via compiler CLI flag `--gpu-min-work`."
   GPU_MIN_WORK
}

fn gpu_async(){
   "Returns true when async GPU dispatch is enabled.

   Configure via compiler CLI flag `--gpu-async`."
   GPU_ASYNC
}

fn gpu_fast_math(){
   "Returns true when relaxed GPU math optimizations are enabled.

   Configure via compiler CLI flag `--gpu-fast-math`."
   GPU_FAST_MATH
}

fn gpu_available(){
   "Returns true when the selected GPU backend appears available on this host."
   GPU_AVAILABLE
}

fn gpu_offload_status(work_items=0){
   "Returns an offload decision map for `work_items`.

   Keys:
   - `backend`: configured backend
   - `selected_backend`: backend chosen after auto-resolution
   - `available`: backend availability on host
   - `policy_selected`: whether policy picks GPU for this workload
   - `active`: true only when a real backend runtime path is active
   - `reason`: policy reason string
   - `active_reason`: runtime activation reason string
   "
   mut out = dict(20)
   mut selected_backend = GPU_BACKEND
   if(selected_backend == "auto"){ selected_backend = _pick_auto_gpu_backend() }
   def min_work_eff = _effective_gpu_min_work()
   mut policy_selected = false
   mut reason = "cpu_default"
   if(GPU_MODE == "off"){
      reason = "gpu_mode_off"
   } elif(GPU_BACKEND == "none"){
      reason = "gpu_backend_none"
   } elif(!GPU_AVAILABLE){
      if(GPU_OFFLOAD == "force"){ reason = "forced_but_backend_unavailable" }
      else { reason = "gpu_backend_unavailable" }
   } elif(GPU_OFFLOAD == "off"){
      reason = "offload_mode_off"
   } elif(GPU_OFFLOAD == "force"){
      policy_selected = true
      reason = "forced"
   } else {
      if(work_items > 0 && work_items < min_work_eff){
         reason = "below_min_work"
      } else {
         policy_selected = true
         reason = "eligible"
      }
   }
   ;; Backend runtime dispatch is not integrated yet, so active path is CPU fallback.
   mut active = false
   mut active_reason = "runtime_backend_unimplemented"
   if(!policy_selected){ active_reason = "policy_not_selected" }
   out = dict_set(out, "mode", GPU_MODE)
   out = dict_set(out, "backend", GPU_BACKEND)
   out = dict_set(out, "selected_backend", selected_backend)
   out = dict_set(out, "offload", GPU_OFFLOAD)
   out = dict_set(out, "available", GPU_AVAILABLE)
   out = dict_set(out, "min_work", GPU_MIN_WORK)
   out = dict_set(out, "effective_min_work", min_work_eff)
   out = dict_set(out, "work_items", work_items)
   out = dict_set(out, "async", GPU_ASYNC)
   out = dict_set(out, "fast_math", GPU_FAST_MATH)
   out = dict_set(out, "policy_selected", policy_selected)
   out = dict_set(out, "active", active)
   out = dict_set(out, "reason", reason)
   out = dict_set(out, "active_reason", active_reason)
   out
}

fn gpu_should_offload(work_items=0){
   "Returns true when offload policy selects GPU for `work_items`."
   dict_get(gpu_offload_status(work_items), "policy_selected", false)
}

fn accel_target(){
   "Returns the selected accelerator target: `none|nvptx|amdgpu|spirv|hsaco`.

   Configure via compiler CLI flag `--accel-target`."
   _resolve_accel_target("")
}

fn accel_targets(){
   "Returns canonical accelerator targets ordered by current host preference."
   mut xs = list(8)
   def pref = accel_target()
   if(pref != "none" && !_list_has(xs, pref)){ xs = append(xs, pref) }
   if(!_list_has(xs, "nvptx")){ xs = append(xs, "nvptx") }
   if(!_list_has(xs, "amdgpu")){ xs = append(xs, "amdgpu") }
   if(!_list_has(xs, "spirv")){ xs = append(xs, "spirv") }
   if(!_list_has(xs, "hsaco")){ xs = append(xs, "hsaco") }
   xs
}

fn accel_target_triple(target=""){
   "Returns LLVM-style target triple for the resolved accelerator target."
   def t = _resolve_accel_target(target)
   if(t == "nvptx"){ return "nvptx64-nvidia-cuda" }
   if(t == "amdgpu" || t == "hsaco"){ return "amdgcn-amd-amdhsa" }
   if(t == "spirv"){ return "spirv64-unknown-unknown" }
   "none"
}

fn accel_binary_kind(target=""){
   "Returns emitted device binary kind: `ptx|o|spv|hsaco|none`."
   if(ACCEL_OBJECT != "auto"){ return ACCEL_OBJECT }
   def t = _resolve_accel_target(target)
   if(t == "nvptx"){ return "ptx" }
   if(t == "amdgpu"){ return "o" }
   if(t == "spirv"){ return "spv" }
   if(t == "hsaco"){ return "hsaco" }
   "none"
}

fn accel_binary_ext(target=""){
   "Returns suggested file extension for emitted device artifact."
   def k = accel_binary_kind(target)
   if(k == "ptx"){ return ".ptx" }
   if(k == "o"){ return ".o" }
   if(k == "spv"){ return ".spv" }
   if(k == "hsaco"){ return ".hsaco" }
   ""
}

fn accel_target_available(target=""){
   "Returns true when runtime or toolchain for target appears available."
   def t = _resolve_accel_target(target)
   _target_runtime_available(t) || _target_toolchain_available(t)
}

fn accel_target_status(target=""){
   "Returns accelerator target status map including availability and artifact details."
   mut out = dict(20)
   def configured = ACCEL_TARGET
   def selected = _resolve_accel_target(target)
   def runtime_ok = _target_runtime_available(selected)
   def toolchain_ok = _target_toolchain_available(selected)
   def available = runtime_ok || toolchain_ok
   mut reason = "none_selected"
   if(selected != "none"){
      if(runtime_ok && toolchain_ok){ reason = "ready_runtime_and_toolchain" }
      elif(toolchain_ok){ reason = "toolchain_only" }
      elif(runtime_ok){ reason = "runtime_only" }
      else { reason = "runtime_and_toolchain_missing" }
   }
   out = dict_set(out, "configured_target", configured)
   out = dict_set(out, "selected_target", selected)
   out = dict_set(out, "triple", accel_target_triple(selected))
   out = dict_set(out, "object_kind", accel_binary_kind(selected))
   out = dict_set(out, "object_ext", accel_binary_ext(selected))
   out = dict_set(out, "runtime_available", runtime_ok)
   out = dict_set(out, "toolchain_available", toolchain_ok)
   out = dict_set(out, "available", available)
   out = dict_set(out, "gpu_backend", GPU_BACKEND)
   out = dict_set(out, "gpu_available", GPU_AVAILABLE)
   out = dict_set(out, "reason", reason)
   out
}

fn accel_compile_plan(input_path, output_path="", target=""){
   "Returns a best-effort device compilation plan map for selected accelerator target.

   Keys:
   - `target`, `triple`, `object_kind`, `object_ext`
   - `input`, `output`
   - `command` (argv-style list)
   - `status` (same shape as `accel_target_status`)
   "
   mut out = dict(20)
   def t = _resolve_accel_target(target)
   def triple = accel_target_triple(t)
   def kind = accel_binary_kind(t)
   def ext = accel_binary_ext(t)
   mut out_path = output_path
   if(!is_str(out_path) || str_len(strip(out_path)) == 0){ out_path = "device" + ext }

   def cc = _env_str_or("NYTRIX_ACCEL_CLANG", "clang")
   def spv_tool = _env_str_or("NYTRIX_ACCEL_LLVM_SPIRV", "llvm-spirv")
   def opt = _env_str_or("NYTRIX_ACCEL_OPT", "3")
   def nv_arch = _env_str_or("NYTRIX_ACCEL_ARCH_NVPTX", "sm_80")
   def amd_arch = _env_str_or("NYTRIX_ACCEL_ARCH_AMDGPU", "gfx1100")

   mut cmd = list(20)
   if(t == "nvptx"){
      cmd = append(cmd, cc)
      cmd = append(cmd, "-target")
      cmd = append(cmd, "nvptx64-nvidia-cuda")
      cmd = append(cmd, "--cuda-gpu-arch=" + nv_arch)
      cmd = append(cmd, "-O" + opt)
      cmd = append(cmd, "-S")
      cmd = append(cmd, input_path)
      cmd = append(cmd, "-o")
      cmd = append(cmd, out_path)
   } elif(t == "amdgpu"){
      cmd = append(cmd, cc)
      cmd = append(cmd, "-target")
      cmd = append(cmd, "amdgcn-amd-amdhsa")
      cmd = append(cmd, "-mcpu=" + amd_arch)
      cmd = append(cmd, "-O" + opt)
      cmd = append(cmd, "-c")
      cmd = append(cmd, input_path)
      cmd = append(cmd, "-o")
      cmd = append(cmd, out_path)
   } elif(t == "spirv"){
      if(endswith(lower(input_path), ".bc")){
         cmd = append(cmd, spv_tool)
         cmd = append(cmd, input_path)
         cmd = append(cmd, "-o")
         cmd = append(cmd, out_path)
      } else {
         cmd = append(cmd, cc)
         cmd = append(cmd, "-target")
         cmd = append(cmd, "spirv64-unknown-unknown")
         cmd = append(cmd, "-O" + opt)
         cmd = append(cmd, "-c")
         cmd = append(cmd, input_path)
         cmd = append(cmd, "-o")
         cmd = append(cmd, out_path)
      }
   } elif(t == "hsaco"){
      cmd = append(cmd, cc)
      cmd = append(cmd, "-target")
      cmd = append(cmd, "amdgcn-amd-amdhsa")
      cmd = append(cmd, "--offload-arch=" + amd_arch)
      cmd = append(cmd, "-O" + opt)
      cmd = append(cmd, "-c")
      cmd = append(cmd, input_path)
      cmd = append(cmd, "-o")
      cmd = append(cmd, out_path)
   }

   out = dict_set(out, "target", t)
   out = dict_set(out, "triple", triple)
   out = dict_set(out, "object_kind", kind)
   out = dict_set(out, "object_ext", ext)
   out = dict_set(out, "input", input_path)
   out = dict_set(out, "output", out_path)
   out = dict_set(out, "command", cmd)
   out = dict_set(out, "status", accel_target_status(t))
   out
}

fn parallel_mode(){
   "Returns the configured parallel mode: `off`, `auto`, or `threads`.

   Configure via compiler CLI flag `--parallel`."
   PARALLEL_MODE
}

fn parallel_threads(){
   "Returns configured thread budget; `0` means runtime/default auto sizing.

   Configure via compiler CLI flag `--threads`."
   PARALLEL_THREADS
}

fn parallel_min_work(){
   "Returns minimum work threshold before selecting threaded parallel execution.

   Configure via compiler CLI flag `--parallel-min-work`."
   PARALLEL_MIN_WORK
}

fn parallel_status(work_items=0){
   "Returns a threading decision map for `work_items`."
   mut out = dict(16)
   def threads_eff = _effective_parallel_threads()
   def min_work_eff = _effective_parallel_min_work()
   mut selected = false
   mut reason = "cpu_default"
   if(PARALLEL_MODE == "off"){
      reason = "parallel_mode_off"
   } elif(threads_eff <= 1){
      reason = "single_thread_budget"
   } elif(work_items > 0 && work_items < min_work_eff){
      reason = "below_min_work"
   } else {
      selected = true
      reason = "eligible"
   }
   out = dict_set(out, "mode", PARALLEL_MODE)
   out = dict_set(out, "threads", PARALLEL_THREADS)
   out = dict_set(out, "effective_threads", threads_eff)
   out = dict_set(out, "min_work", PARALLEL_MIN_WORK)
   out = dict_set(out, "effective_min_work", min_work_eff)
   out = dict_set(out, "work_items", work_items)
   out = dict_set(out, "selected", selected)
   out = dict_set(out, "reason", reason)
   out
}

fn parallel_should_threads(work_items=0){
   "Returns true when thread-parallel policy selects threaded execution."
   dict_get(parallel_status(work_items), "selected", false)
}

fn getcwd(){
   "Returns the current working directory as a string; returns `\"\"` if `getcwd(2)` fails."
   mut buf = malloc(4096)
   mut clen = __getcwd(buf, 4096)
   if(clen <= 0){
      free(buf)
      ""
   } else {
      def s = cstr_to_str(buf)
      free(buf)
      s
   }
}

fn uid(){
   "Returns the **real user ID** of the calling process via **getuid(2)**."
   __getuid()
}

fn gid(){
   "Returns the **real group ID** of the calling process via **getgid(2)**."
   __getgid()
}

fn file_read(path) -> Result {
   "Reads the whole file at `path`; returns `ok(content_string)` or `err(errno_like_code)`."
   def p = ospath.normalize(path)
   match sys_open(p, 0, 0){ ; O_RDONLY
      ok(fd) -> {
         defer { unwrap(sys_close(fd)) }
         mut cap = 4096
         mut buf = malloc(cap)
         def tmp = malloc(4096)
         defer { free(tmp) }
         mut tlen = 0
         while(true){
            match sys_read(fd, tmp, 4096){
               ok(r) -> {
                  if(r <= 0){ break }
                  if(tlen + r >= cap){
                     while(tlen + r >= cap){ cap = cap * 2 }
                     buf = realloc(buf, cap)
                  }
                  __copy_mem(buf + tlen, tmp, r)
                  tlen = tlen + r
               }
               err(e) -> { return err(e) }
            }
         }
         store8(buf, 0, tlen)
         return ok(init_str(buf, tlen))
      }
      err(e) -> { return err(e) }
   }
}

fn file_write(path, content) -> Result {
   "Writes `content` to `path` (truncate/create); returns `ok(bytes_written)` or `err(errno_like_code)`."
   def p = ospath.normalize(path)
   def open_res = _open_with_retry(p, 577, 420) ;; WRONLY|CREAT|TRUNC, 0644
   if(is_err(open_res)){ return open_res }
   def fd = unwrap(open_res)
   defer { unwrap(sys_close(fd)) }
   def n = str_len(content)
   return sys_write(fd, content, n)
}

fn file_exists(path){
   "Returns true when `path` exists (file or directory)."
   def p = ospath.normalize(path)
   mut res = __access(p, 0)
   res == 0
}

fn file_append(path, content) -> Result {
   "Appends `content` to `path` (create if missing); returns `ok(bytes_written)` or `err(errno_like_code)`."
   def p = ospath.normalize(path)
   def open_res = _open_with_retry(p, 1089, 420) ;; WRONLY|CREAT|APPEND, 0644
   if(is_err(open_res)){ return open_res }
   def fd = unwrap(open_res)
   defer { unwrap(sys_close(fd)) }
   def n = str_len(content)
   return sys_write(fd, content, n)
}

fn file_remove(path) -> Result {
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

if(comptime{__main()}){
    use std.os *
    use std.core *
    use std.core.error *
    use std.core.reflect *
    use std.str *
    use std.os.sys *

    print("Testing OS Mod...")

    def p = pid()
    assert(p > 0, "pid > 0")

    def pp = ppid()
    if(eq(os(), "windows")){
        print("Windows ppid:", pp)
        assert(pp >= 0, "Windows ppid should be non-negative")
    } else {
        assert(pp > 0, "ppid > 0")
    }

    def u = uid()
    assert(u >= 0, "uid >= 0")

    def g = gid()
    assert(g >= 0, "gid >= 0")

    def path = env("PATH")
    if(path != 0){
     assert(str_len(path) >= 0, "env PATH len")
    } else {
     assert(0, "env PATH missing")
    }

    def e = environ()
    assert(type(e) == "list", "environ list")
    assert(len(e) > 0, "environ len")

    ;; Platform tests
    def o = os()
    assert(is_str(o), "os() is string")
    assert(len(o) > 0, "os() not empty")

    def a = arch()
    assert(is_str(a), "arch() is string")
    assert(len(a) > 0, "arch() not empty")

    print("Platform: " + o + " (" + a + ")")

    print("✓ std.os.mod tests passed")
}
