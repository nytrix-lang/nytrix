;; Keywords: accel acceleration offload os
;; Accelerator policy facade.
;; References:
;; - std.os
module std.os.accel(gpu_mode, gpu_backend, gpu_offload, gpu_min_work, gpu_async, gpu_fast_math, gpu_available, gpu_should_offload, gpu_offload_status, accel_target, accel_targets, accel_target_available, accel_target_triple, accel_binary_kind, accel_binary_ext, accel_target_status, accel_compile_plan, opencl_available, opencl_toolchain_available, opencl_async, opencl_fast_math, opencl_should_offload, opencl_status, opencl_device_policy, opencl_compile_plan, opencl_kernel_plan, opencl_cpu_fallback_plan, opencl_dispatch_plan, opencl_work_groups, parallel_mode, parallel_threads, parallel_min_work, parallel_should_threads, parallel_status, GPU_MODE, GPU_BACKEND, GPU_OFFLOAD, GPU_MIN_WORK, GPU_ASYNC, GPU_FAST_MATH, GPU_AVAILABLE, ACCEL_TARGET, ACCEL_OBJECT, OPENCL_AVAILABLE, OPENCL_TOOLCHAIN_AVAILABLE, PARALLEL_MODE, PARALLEL_THREADS, PARALLEL_MIN_WORK)
use std.os.gpu (
   gpu_mode as _os_gpu_mode, gpu_backend as _os_gpu_backend, gpu_offload as _os_gpu_offload,
   gpu_min_work as _os_gpu_min_work, gpu_async as _os_gpu_async, gpu_fast_math as _os_gpu_fast_math,
   gpu_available as _os_gpu_available, gpu_should_offload as _os_gpu_should_offload,
   gpu_offload_status as _os_gpu_offload_status, accel_target as _os_accel_target,
   accel_targets as _os_accel_targets, accel_target_available as _os_accel_target_available,
   accel_target_triple as _os_accel_target_triple, accel_binary_kind as _os_accel_binary_kind,
   accel_binary_ext as _os_accel_binary_ext, accel_target_status as _os_accel_target_status,
   accel_compile_plan as _os_accel_compile_plan, opencl_available as _os_opencl_available,
   opencl_toolchain_available as _os_opencl_toolchain_available, opencl_async as _os_opencl_async,
   opencl_fast_math as _os_opencl_fast_math, opencl_should_offload as _os_opencl_should_offload,
   opencl_status as _os_opencl_status, opencl_device_policy as _os_opencl_device_policy,
   opencl_compile_plan as _os_opencl_compile_plan, opencl_kernel_plan as _os_opencl_kernel_plan,
   opencl_cpu_fallback_plan as _os_opencl_cpu_fallback_plan, opencl_dispatch_plan as _os_opencl_dispatch_plan,
   opencl_work_groups as _os_opencl_work_groups,
)

use std.os.parallel (
   parallel_mode as _os_parallel_mode, parallel_threads as _os_parallel_threads,
   parallel_min_work as _os_parallel_min_work, parallel_should_threads as _os_parallel_should_threads,
   parallel_status as _os_parallel_status,
)

use std.os.gpu as osgpu
use std.core
use std.core.str
use std.core.common as common
use std.os.prim (env)

fn _accel_env_str_or(str key, str fallback) str {
   def v = env(key)
   if is_str(v) {
      def s = strip(v)
      if s.len > 0 { return s }
   }
   fallback
}

fn _accel_bool_or(any v, bool fallback) bool {
   if !is_str(v) { return fallback }
   def s = lower(strip(v))
   case s {
      "1", "true", "yes", "on" -> true
      "0", "false", "no", "off" -> false
      _ -> fallback
   }
}

fn _accel_gpu_mode(str v) str {
   def s = lower(strip(v))
   case s {
      "off", "auto", "opencl" -> s
      _ -> "auto"
   }
}

fn _accel_gpu_backend(str v) str {
   mut s = lower(strip(v))
   if eq(s, "off") { s = "none" }
   case s {
      "none", "auto", "opencl", "cuda", "hip", "metal" -> s
      _ -> "auto"
   }
}

fn _accel_gpu_offload(str v) str {
   mut s = lower(strip(v))
   if eq(s, "true") || eq(s, "yes") { s = "on" }
   if eq(s, "false") || eq(s, "no") { s = "off" }
   case s {
      "off", "auto", "on", "force" -> s
      _ -> "auto"
   }
}

fn _accel_target_const(str v) str {
   mut s = lower(strip(v))
   if eq(s, "off") { s = "none" }
   if eq(s, "cuda") || eq(s, "ptx") { s = "nvptx" } elif eq(s, "hip") || eq(s, "rocm") || eq(s, "gcn") || eq(s, "rdna") { s = "amdgpu" } elif eq(s, "opencl") || eq(s, "vulkan") || eq(s, "spv") { s = "spirv" } elif eq(s, "hsa") || eq(s, "hsa_code_object") || eq(s, "hsa-code-object") || eq(s, "rocm_hsa") { s = "hsaco" }
   case s {
      "none", "auto", "nvptx", "amdgpu", "spirv", "hsaco" -> s
      _ -> "auto"
   }
}

fn _accel_object_const(str v) str {
   mut s = lower(strip(v))
   if eq(s, "obj") { s = "o" }
   if eq(s, "cubin") { s = "ptx" }
   case s {
      "none", "auto", "ptx", "o", "spv", "hsaco" -> s
      _ -> "auto"
   }
}

fn _accel_parallel_mode(str v) str {
   def s = lower(strip(v))
   case s {
      "off", "auto", "threads" -> s
      _ -> "auto"
   }
}

fn _accel_path_exists(str path) bool { __access(path, 0) == 0 }

fn _accel_has_cuda_runtime() bool {
   def vis = _accel_env_str_or("CUDA_VISIBLE_DEVICES", "")
   if eq(vis, "-1") { return false }
   if _accel_env_str_or("CUDA_PATH", "").len > 0 { return true }
   #windows {
      if _accel_path_exists("C:\\Windows\\System32\\nvcuda.dll") { return true }
   } #else {
      if _accel_path_exists("/dev/nvidiactl") { return true }
      if _accel_path_exists("/proc/driver/nvidia/version") { return true }
      if _accel_path_exists("/usr/local/cuda/bin/ptxas") { return true }
      if _accel_path_exists("/usr/local/cuda/bin/nvcc") { return true }
   } #endif
   false
}

fn _accel_has_hip_runtime() bool {
   def vis = _accel_env_str_or("HIP_VISIBLE_DEVICES", "")
   if eq(vis, "-1") { return false }
   if _accel_env_str_or("ROCM_PATH", "").len > 0 { return true }
   if _accel_env_str_or("HIP_PATH", "").len > 0 { return true }
   #linux {
      if _accel_path_exists("/dev/kfd") { return true }
      if _accel_path_exists("/opt/rocm/bin/amdclang") { return true }
      if _accel_path_exists("/opt/rocm/bin/clang") { return true }
   } #endif
   false
}

fn _accel_gpu_available_const() bool {
   def raw = _accel_env_str_or("NYTRIX_GPU_AVAILABLE", "")
   if raw.len > 0 { return _accel_bool_or(raw, false) }
   if eq(GPU_BACKEND, "none") { return false }
   if eq(GPU_BACKEND, "cuda") { return _accel_has_cuda_runtime() }
   if eq(GPU_BACKEND, "hip") { return _accel_has_hip_runtime() }
   if eq(GPU_BACKEND, "opencl") { return osgpu.opencl_available() }
   if eq(GPU_BACKEND, "metal") { #macos { return true } #else { return false } #endif }
   if eq(GPU_BACKEND, "auto") {
      if _accel_has_cuda_runtime() { return true }
      if _accel_has_hip_runtime() { return true }
      if osgpu.opencl_available() { return true }
      #macos { return true } #endif
   }
   false
}

fn gpu_mode() str {
   "Returns the configured GPU mode: `off`, `auto`, or `opencl`."
   _os_gpu_mode()
}

fn gpu_backend() str {
   "Returns configured GPU backend: `none`, `auto`, `opencl`, `cuda`, `hip`, or `metal`."
   _os_gpu_backend()
}

fn gpu_offload() str {
   "Returns GPU offload policy: `off`, `auto`, `on`, or `force`."
   _os_gpu_offload()
}

fn gpu_min_work() int {
   "Returns minimum work threshold before trying GPU offload; `0` means auto/default."
   _os_gpu_min_work()
}

fn gpu_async() bool {
   "Returns true when async GPU dispatch is enabled."
   _os_gpu_async()
}

fn gpu_fast_math() bool {
   "Returns true when relaxed GPU math optimizations are enabled."
   _os_gpu_fast_math()
}

fn gpu_available() bool {
   "Returns true when the selected GPU backend appears available on this host."
   _os_gpu_available()
}

fn gpu_offload_status(int work_items=0) dict {
   "Returns an offload decision map for `work_items`."
   _os_gpu_offload_status(work_items)
}

fn gpu_should_offload(int work_items=0) bool {
   "Returns true when offload policy selects GPU for `work_items`."
   _os_gpu_should_offload(work_items)
}

fn accel_target() str {
   "Returns the selected accelerator target: `none|nvptx|amdgpu|spirv|hsaco`."
   _os_accel_target()
}

fn accel_targets() list {
   "Returns canonical accelerator targets ordered by host preference."
   _os_accel_targets()
}

fn accel_target_available(str target="") bool {
   "Returns true when runtime or toolchain for target appears available."
   _os_accel_target_available(target)
}

fn accel_target_triple(str target="") str {
   "Returns LLVM-style triple for selected accelerator target."
   _os_accel_target_triple(target)
}

fn accel_binary_kind(str target="") str {
   "Returns emitted device artifact kind: `ptx|o|spv|hsaco|none`."
   _os_accel_binary_kind(target)
}

fn accel_binary_ext(str target="") str {
   "Returns suggested file extension for emitted device artifact."
   _os_accel_binary_ext(target)
}

fn accel_target_status(str target="") dict {
   "Returns accelerator target status map."
   _os_accel_target_status(target)
}

fn accel_compile_plan(str input_path, str output_path="", str target="") dict {
   "Returns best-effort device compilation command plan for chosen target."
   _os_accel_compile_plan(input_path, output_path, target)
}

fn opencl_available() bool {
   "Returns true when an OpenCL runtime appears available."
   _os_opencl_available()
}

fn opencl_toolchain_available() bool {
   "Returns true when a SPIR-V/OpenCL-capable toolchain appears available."
   _os_opencl_toolchain_available()
}

fn opencl_async() bool {
   "Returns the configured async GPU dispatch preference."
   _os_opencl_async()
}

fn opencl_fast_math() bool {
   "Returns the configured relaxed math preference for accelerator code."
   _os_opencl_fast_math()
}

fn opencl_status(int work_items=0) dict {
   "Returns OpenCL policy, availability, and CPU fallback metadata."
   _os_opencl_status(work_items)
}

fn opencl_device_policy(int work_items=0) dict {
   "Alias for opencl_status; useful at call sites that choose CPU/GPU plans."
   _os_opencl_device_policy(work_items)
}

fn opencl_compile_plan(str input_path, str output_path="") dict {
   "Returns a SPIR-V compile command plan for an LLVM IR input."
   _os_opencl_compile_plan(input_path, output_path)
}

fn opencl_kernel_plan(str name, int global_size, int local_size=0) dict {
   "Returns normalized launch-shape metadata for an OpenCL-style kernel."
   _os_opencl_kernel_plan(name, global_size, local_size)
}

fn opencl_cpu_fallback_plan(int work_items=0, int item_cost=1) dict {
   "Returns the CPU plan used when OpenCL is unavailable or not selected."
   _os_opencl_cpu_fallback_plan(work_items, item_cost)
}

fn opencl_dispatch_plan(str name, int global_size, int local_size=0) dict {
   "Returns the full OpenCL kernel plan plus the CPU fallback plan."
   _os_opencl_dispatch_plan(name, global_size, local_size)
}

fn opencl_work_groups(int global_size, int local_size=0) int {
   "Returns the number of work groups for `global_size` and optional `local_size`."
   _os_opencl_work_groups(global_size, local_size)
}

fn opencl_should_offload(int work_items=0) bool {
   "Returns true when OpenCL policy selects device execution."
   _os_opencl_should_offload(work_items)
}

fn parallel_mode() str {
   "Returns the configured parallel mode: `off`, `auto`, or `threads`."
   _os_parallel_mode()
}

fn parallel_threads() int {
   "Returns configured thread budget; `0` means runtime/default auto sizing."
   _os_parallel_threads()
}

fn parallel_min_work() int {
   "Returns minimum work threshold before selecting threaded parallel execution."
   _os_parallel_min_work()
}

fn parallel_status(int work_items=0) dict {
   "Returns a threading decision map for `work_items`."
   _os_parallel_status(work_items)
}

fn parallel_should_threads(int work_items=0) bool {
   "Returns true when thread-parallel policy selects threaded execution."
   _os_parallel_should_threads(work_items)
}

def GPU_MODE = _accel_gpu_mode(_accel_env_str_or("NYTRIX_GPU_MODE", "auto"))
def GPU_BACKEND = _accel_gpu_backend(_accel_env_str_or("NYTRIX_GPU_BACKEND", "auto"))
def GPU_OFFLOAD = _accel_gpu_offload(_accel_env_str_or("NYTRIX_GPU_OFFLOAD", "auto"))
def GPU_MIN_WORK = common.parse_nonneg_int(env("NYTRIX_GPU_MIN_WORK"))
def GPU_ASYNC = _accel_bool_or(env("NYTRIX_GPU_ASYNC"), true)
def GPU_FAST_MATH = _accel_bool_or(env("NYTRIX_GPU_FAST_MATH"), false)
def GPU_AVAILABLE = _accel_gpu_available_const()
def ACCEL_TARGET = _accel_target_const(_accel_env_str_or("NYTRIX_ACCEL_TARGET", "auto"))
def ACCEL_OBJECT = _accel_object_const(_accel_env_str_or("NYTRIX_ACCEL_OBJECT", "auto"))
def OPENCL_AVAILABLE = osgpu.opencl_available()
def OPENCL_TOOLCHAIN_AVAILABLE = osgpu.opencl_toolchain_available()
def PARALLEL_MODE = _accel_parallel_mode(_accel_env_str_or("NYTRIX_PARALLEL_MODE", "auto"))
def PARALLEL_THREADS = common.parse_nonneg_int(env("NYTRIX_PARALLEL_THREADS"))
def PARALLEL_MIN_WORK = common.parse_nonneg_int(env("NYTRIX_PARALLEL_MIN_WORK"))
