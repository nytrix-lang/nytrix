;; Keywords: gpu graphics compute os
;; GPU and Accelerator policy.
;; References:
;; - std.os
module std.os.gpu(gpu_mode, gpu_backend, gpu_offload, gpu_min_work, gpu_async, gpu_fast_math, gpu_available, gpu_should_offload, gpu_offload_status, accel_target, accel_targets, accel_target_available, accel_target_triple, accel_binary_kind, accel_binary_ext, accel_backend, accel_target_status, accel_compile_plan, accel_emit_plan, accel_emit_command, opencl_available, opencl_toolchain_available, opencl_async, opencl_fast_math, opencl_should_offload, opencl_status, opencl_device_policy, opencl_compile_plan, opencl_kernel_plan, opencl_cpu_fallback_plan, opencl_dispatch_plan, opencl_work_groups, GPU_MODE, GPU_BACKEND, GPU_OFFLOAD, GPU_MIN_WORK, GPU_ASYNC, GPU_FAST_MATH, GPU_AVAILABLE, ACCEL_TARGET, ACCEL_OBJECT, OPENCL_AVAILABLE, OPENCL_TOOLCHAIN_AVAILABLE)
use std.core
use std.core.str
use std.os.prim
use std.core.common as common

fn _env_str_or(str key, str fallback) str {
   def v = env(key)
   mut out = fallback
   if is_str(v) {
      def s = strip(v)
      if s.len > 0 { out = s }
   }
   out
}

fn _parse_bool_or(any v, bool fallback) bool {
   if !is_str(v) { return fallback }
   def s = lower(strip(v))
   return case s {
      "1", "true", "yes", "on" -> true
      "0", "false", "no", "off" -> false
      _ -> fallback
   }
}

fn _path_exists(str path) bool { __access(path, 0) == 0 }

fn _normalize_gpu_mode(any v) str {
   if !is_str(v) { return "auto" }
   def s = lower(strip(v))
   case s {
      "off", "auto", "opencl" -> s
      _ -> "auto"
   }
}

fn _normalize_gpu_backend(any v) str {
   if !is_str(v) { return "auto" }
   mut s = lower(strip(v))
   if eq(s, "off") { s = "none" }
   case s {
      "none", "auto", "opencl", "cuda", "hip", "metal" -> s
      _ -> "auto"
   }
}

fn _normalize_gpu_offload(any v) str {
   if !is_str(v) { return "auto" }
   mut s = lower(strip(v))
   if eq(s, "true") || eq(s, "yes") { s = "on" }
   if eq(s, "false") || eq(s, "no") { s = "off" }
   case s {
      "off", "auto", "on", "force" -> s
      _ -> "auto"
   }
}

fn _normalize_accel_target(any v) str {
   if !is_str(v) { return "auto" }
   mut s = lower(strip(v))
   if eq(s, "off") { s = "none" }
   if eq(s, "cuda") || eq(s, "ptx") { s = "nvptx" }
   elif eq(s, "hip") || eq(s, "rocm") || eq(s, "gcn") || eq(s, "rdna") { s = "amdgpu" }
   elif eq(s, "opencl") || eq(s, "vulkan") || eq(s, "spv") { s = "spirv" }
   elif eq(s, "hsa") || eq(s, "hsa_code_object") || eq(s, "hsa-code-object") || eq(s, "rocm_hsa") { s = "hsaco" }
   case s {
      "none", "auto", "nvptx", "amdgpu", "spirv", "hsaco" -> s
      _ -> "auto"
   }
}

fn _normalize_accel_object(any v) str {
   if !is_str(v) { return "auto" }
   mut s = lower(strip(v))
   if s == "obj" { s = "o" }
   if s == "cubin" { s = "ptx" }
   return case s {
      "none", "auto", "ptx", "o", "spv", "hsaco" -> s
      _ -> "auto"
   }
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

fn _has_opencl_runtime() bool {
   if _opencl_runtime_loaded { return _opencl_runtime_cache }
   def force = env("NYTRIX_OPENCL_FORCE")
   if is_str(force) && strip(force).len > 0 {
      _opencl_runtime_cache = _parse_bool_or(force, false)
      _opencl_runtime_loaded = true
      return _opencl_runtime_cache
   }
   mut out = false
   if IS_WINDOWS {
      if _path_exists("C:\\Windows\\System32\\OpenCL.dll") { out = true }
      elif _path_exists("C:\\Windows\\SysWOW64\\OpenCL.dll") { out = true }
      _opencl_runtime_cache = out
      _opencl_runtime_loaded = true
      return out
   }
   if IS_MACOS {
      out = _path_exists("/System/Library/Frameworks/OpenCL.framework/OpenCL")
      _opencl_runtime_cache = out
      _opencl_runtime_loaded = true
      return out
   }
   if _path_exists("/etc/OpenCL/vendors") { out = true }
   elif _path_exists("/usr/lib/libOpenCL.so") { out = true }
   elif _path_exists("/usr/lib64/libOpenCL.so") { out = true }
   elif _path_exists("/usr/local/lib/libOpenCL.so") { out = true }
   elif _path_exists("/lib/x86_64-linux-gnu/libOpenCL.so.1") { out = true }
   elif _path_exists("/usr/lib/x86_64-linux-gnu/libOpenCL.so.1") { out = true }
   _opencl_runtime_cache = out
   _opencl_runtime_loaded = true
   out
}

fn _has_cuda_runtime() bool {
   if _cuda_runtime_loaded { return _cuda_runtime_cache }
   def vis = _env_str_or("CUDA_VISIBLE_DEVICES", "")
   if vis == "-1" {
      _cuda_runtime_cache = false
      _cuda_runtime_loaded = true
      return false
   }
   mut out = false
   if IS_WINDOWS {
      if _path_exists("C:\\Windows\\System32\\nvcuda.dll") { out = true }
      def cp = _env_str_or("CUDA_PATH", "")
      if cp.len > 0 { out = true }
      _cuda_runtime_cache = out
      _cuda_runtime_loaded = true
      return out
   }
   if _path_exists("/dev/nvidiactl") { out = true }
   elif _path_exists("/proc/driver/nvidia/version") { out = true }
   def cp = _env_str_or("CUDA_PATH", "")
   if cp.len > 0 { out = true }
   _cuda_runtime_cache = out
   _cuda_runtime_loaded = true
   out
}

fn _has_hip_runtime() bool {
   if _hip_runtime_loaded { return _hip_runtime_cache }
   def vis = _env_str_or("HIP_VISIBLE_DEVICES", "")
   if vis == "-1" {
      _hip_runtime_cache = false
      _hip_runtime_loaded = true
      return false
   }
   mut out = false
   if _path_exists("/dev/kfd") { out = true }
   def rp = _env_str_or("ROCM_PATH", "")
   if rp.len > 0 { out = true }
   def hp = _env_str_or("HIP_PATH", "")
   if hp.len > 0 { out = true }
   _hip_runtime_cache = out
   _hip_runtime_loaded = true
   out
}

fn _has_nvptx_toolchain() bool {
   if _nvptx_toolchain_loaded { return _nvptx_toolchain_cache }
   mut out = false
   def cp = _env_str_or("CUDA_PATH", "")
   if cp.len > 0 { out = true }
   elif _path_exists("/usr/local/cuda/bin/ptxas") { out = true }
   elif _path_exists("/usr/local/cuda/bin/nvcc") { out = true }
   elif _path_exists("/usr/bin/ptxas") { out = true }
   elif _path_exists("/usr/bin/nvcc") { out = true }
   _nvptx_toolchain_cache = out
   _nvptx_toolchain_loaded = true
   out
}

fn _has_amdgpu_toolchain() bool {
   if _amdgpu_toolchain_loaded { return _amdgpu_toolchain_cache }
   mut out = false
   def rp = _env_str_or("ROCM_PATH", "")
   if rp.len > 0 { out = true }
   def hp = _env_str_or("HIP_PATH", "")
   if hp.len > 0 { out = true }
   elif _path_exists("/opt/rocm/bin/amdclang") { out = true }
   elif _path_exists("/opt/rocm/bin/clang") { out = true }
   elif _path_exists("/opt/rocm/llvm/bin/llc") { out = true }
   _amdgpu_toolchain_cache = out
   _amdgpu_toolchain_loaded = true
   out
}

fn _has_spirv_toolchain() bool {
   if _spirv_toolchain_loaded { return _spirv_toolchain_cache }
   mut out = false
   if _path_exists("/usr/bin/llvm-spirv") { out = true }
   elif _path_exists("/usr/local/bin/llvm-spirv") { out = true }
   def sdk = _env_str_or("VULKAN_SDK", "")
   if sdk.len > 0 { out = true }
   elif _has_opencl_runtime() { out = true }
   _spirv_toolchain_cache = out
   _spirv_toolchain_loaded = true
   out
}

fn _has_hsaco_toolchain() bool {
   if _hsaco_toolchain_loaded { return _hsaco_toolchain_cache }
   mut out = false
   if _has_amdgpu_toolchain() { if _path_exists("/opt/rocm/bin/clang") || _path_exists("/opt/rocm/bin/amdclang") { out = true } }
   _hsaco_toolchain_cache = out
   _hsaco_toolchain_loaded = true
   out
}

fn _backend_to_accel_target(any backend) str {
   def b = _normalize_gpu_backend(backend)
   if b == "cuda" { return "nvptx" }
   if b == "hip" { return "hsaco" }
   if b == "opencl" { return "spirv" }
   "none"
}

fn _pick_auto_accel_target() str {
   if _auto_accel_target_loaded { return _auto_accel_target_cache }
   mut out = "none"
   def bt = _backend_to_accel_target(GPU_BACKEND)
   if bt != "none" && bt != "auto" { out = bt }
   elif _has_cuda_runtime() || _has_nvptx_toolchain() { out = "nvptx" }
   elif _has_hip_runtime() || _has_hsaco_toolchain() { out = "hsaco" }
   elif _has_opencl_runtime() || _has_spirv_toolchain() { out = "spirv" }
   elif _has_amdgpu_toolchain() { out = "amdgpu" }
   _auto_accel_target_cache = out
   _auto_accel_target_loaded = true
   out
}

fn _resolve_accel_target(any target) str {
   mut t = ACCEL_TARGET
   if is_str(target) && strip(target).len > 0 { t = _normalize_accel_target(target) }
   if t == "auto" { t = _pick_auto_accel_target() }
   if t == "none" {
      def bt = _backend_to_accel_target(GPU_BACKEND)
      if bt != "none" { t = bt }
   }
   t
}

fn _target_runtime_available(any target) bool {
   def t = _normalize_accel_target(target)
   if t == "nvptx" { return _has_cuda_runtime() }
   if t == "amdgpu" { return _has_hip_runtime() || _path_exists("/dev/dri/renderD128") }
   if t == "spirv" { return _has_opencl_runtime() }
   if t == "hsaco" { return _has_hip_runtime() && _path_exists("/dev/kfd") }
   false
}

fn _target_toolchain_available(any target) bool {
   def t = _normalize_accel_target(target)
   if t == "nvptx" { return _has_nvptx_toolchain() }
   if t == "amdgpu" { return _has_amdgpu_toolchain() }
   if t == "spirv" { return _has_spirv_toolchain() }
   if t == "hsaco" { return _has_hsaco_toolchain() }
   false
}

fn _pick_auto_gpu_backend() str {
   if _auto_gpu_backend_loaded { return _auto_gpu_backend_cache }
   mut out = "none"
   if _has_cuda_runtime() { out = "cuda" }
   elif _has_hip_runtime() { out = "hip" }
   elif _has_opencl_runtime() { out = "opencl" }
   elif IS_MACOS { out = "metal" }
   _auto_gpu_backend_cache = out
   _auto_gpu_backend_loaded = true
   out
}

fn _gpu_backend_available(any backend) bool {
   def b = _normalize_gpu_backend(backend)
   if b == "none" { return false }
   if b == "auto" { return _pick_auto_gpu_backend() != "none" }
   if b == "opencl" { return _has_opencl_runtime() }
   if b == "cuda" { return _has_cuda_runtime() }
   if b == "hip" { return _has_hip_runtime() }
   if b == "metal" { return IS_MACOS }
   false
}

fn _gpu_available_from_env() int {
   def raw = _env_str_or("NYTRIX_GPU_AVAILABLE", "")
   if raw.len == 0 { return -1 }
   if _parse_bool_or(raw, false) { return 1 }
   0
}

fn _compute_gpu_available() bool {
   def ov = _gpu_available_from_env()
   ov == 1 || (ov != 0 && _gpu_backend_available(GPU_BACKEND))
}

fn _effective_gpu_min_work() int {
   if GPU_MIN_WORK > 0 { return GPU_MIN_WORK }
   if GPU_BACKEND == "cuda" || GPU_BACKEND == "hip" || GPU_BACKEND == "metal" { return 2048 }
   4096
}

def GPU_MODE = _normalize_gpu_mode(_env_str_or("NYTRIX_GPU_MODE", "auto"))
def GPU_BACKEND = _normalize_gpu_backend(_env_str_or("NYTRIX_GPU_BACKEND", "auto"))
def GPU_OFFLOAD = _normalize_gpu_offload(_env_str_or("NYTRIX_GPU_OFFLOAD", "auto"))
def GPU_MIN_WORK = common.parse_nonneg_int(env("NYTRIX_GPU_MIN_WORK"))
def GPU_ASYNC = _parse_bool_or(env("NYTRIX_GPU_ASYNC"), true)
def GPU_FAST_MATH = _parse_bool_or(env("NYTRIX_GPU_FAST_MATH"), false)
def GPU_AVAILABLE = _compute_gpu_available()
def ACCEL_TARGET = _normalize_accel_target(_env_str_or("NYTRIX_ACCEL_TARGET", "auto"))
def ACCEL_OBJECT = _normalize_accel_object(_env_str_or("NYTRIX_ACCEL_OBJECT", "auto"))

fn gpu_mode() str {
   "Returns the configured GPU mode: `off`, `auto`, or `opencl`.
   Configure via compiler CLI flag `--gpu`."
   GPU_MODE
}

fn gpu_backend() str {
   "Returns configured GPU backend: `none`, `auto`, `opencl`, `cuda`, `hip`, or `metal`.
   Configure via compiler CLI flag `--gpu-backend`."
   GPU_BACKEND
}

fn gpu_offload() str {
   "Returns GPU offload policy: `off`, `auto`, `on`, or `force`.
   Configure via compiler CLI flag `--gpu-offload`."
   GPU_OFFLOAD
}

fn gpu_min_work() int {
   "Returns minimum work threshold before trying GPU offload; `0` means auto/default.
   Configure via compiler CLI flag `--gpu-min-work`."
   GPU_MIN_WORK
}

fn gpu_async() bool {
   "Returns true when async GPU dispatch is enabled.
   Configure via compiler CLI flag `--gpu-async`."
   GPU_ASYNC
}

fn gpu_fast_math() bool {
   "Returns true when relaxed GPU math optimizations are enabled.
   Configure via compiler CLI flag `--gpu-fast-math`."
   GPU_FAST_MATH
}

fn gpu_available() bool {
   "Returns true when the selected GPU backend appears available on this host."
   GPU_AVAILABLE
}

fn gpu_offload_status(int work_items=0) dict {
   "Returns an offload decision map for `work_items`."
   mut selected_backend = GPU_BACKEND
   if selected_backend == "auto" { selected_backend = _pick_auto_gpu_backend() }
   def min_work_eff = _effective_gpu_min_work()
   mut policy_selected = false
   mut reason = "cpu_default"
   if GPU_MODE == "off" { reason = "gpu_mode_off" } elif GPU_BACKEND == "none" {
      reason = "gpu_backend_none"
   } elif !GPU_AVAILABLE {
      if GPU_OFFLOAD == "force" { reason = "forced_but_backend_unavailable" }
      else { reason = "gpu_backend_unavailable" }
   } elif GPU_OFFLOAD == "off" {
      reason = "offload_mode_off"
   } elif GPU_OFFLOAD == "force" {
      policy_selected = true
      reason = "forced"
   } else {
      if work_items > 0 && work_items < min_work_eff { reason = "below_min_work" } else {
         policy_selected = true
         reason = "eligible"
      }
   }
   mut active = false
   mut active_reason = "runtime_backend_unimplemented"
   if !policy_selected { active_reason = "policy_not_selected" }
   {"mode": GPU_MODE, "backend": GPU_BACKEND, "selected_backend": selected_backend, "offload": GPU_OFFLOAD,
      "available": GPU_AVAILABLE, "min_work": GPU_MIN_WORK, "effective_min_work": min_work_eff,
      "work_items": work_items, "async": GPU_ASYNC, "fast_math": GPU_FAST_MATH, "policy_selected": policy_selected,
   "active": active, "reason": reason, "active_reason": active_reason}
}

fn gpu_should_offload(int work_items=0) bool {
   "Returns true when offload policy selects GPU for `work_items`."
   gpu_offload_status(work_items).get("policy_selected", false)
}

fn accel_target() str {
   "Returns the selected accelerator target: `none|nvptx|amdgpu|spirv|hsaco`.
   Configure via compiler CLI flag `--accel-target`."
   _resolve_accel_target("")
}

fn accel_targets() list {
   "Returns canonical accelerator targets ordered by current host preference."
   mut xs = list(8)
   def pref = accel_target()
   if pref != "none" && !xs.contains(pref) { xs = xs.append(pref) }
   if !xs.contains("nvptx") { xs = xs.append("nvptx") }
   if !xs.contains("amdgpu") { xs = xs.append("amdgpu") }
   if !xs.contains("spirv") { xs = xs.append("spirv") }
   if !xs.contains("hsaco") { xs = xs.append("hsaco") }
   xs
}

fn accel_target_triple(any target="") str {
   "Returns backend target triple for the resolved accelerator target."
   def t = _resolve_accel_target(target)
   if t == "nvptx" { return "nvptx64-nvidia-cuda" }
   if t == "amdgpu" || t == "hsaco" { return "amdgcn-amd-amdhsa" }
   if t == "spirv" { return "spirv64-unknown-unknown" }
   "none"
}

fn accel_binary_kind(any target="") str {
   "Returns emitted device binary kind: `ptx|o|spv|hsaco|none`."
   if ACCEL_OBJECT != "auto" { return ACCEL_OBJECT }
   def t = _resolve_accel_target(target)
   if t == "nvptx" { return "ptx" }
   if t == "amdgpu" { return "o" }
   if t == "spirv" { return "spv" }
   if t == "hsaco" { return "hsaco" }
   "none"
}

fn accel_binary_ext(any target="") str {
   "Returns suggested file extension for emitted device artifact."
   def k = accel_binary_kind(target)
   if k == "ptx" { return ".ptx" }
   if k == "o" { return ".o" }
   if k == "spv" { return ".spv" }
   if k == "hsaco" { return ".hsaco" }
   ""
}

fn accel_backend(any target="") str {
   "Returns the compiler backend family for an accelerator target."
   def t = _resolve_accel_target(target)
   if t == "nvptx" { return "nvptx" }
   if t == "spirv" { return "spirv" }
   if t == "amdgpu" || t == "hsaco" { return "amdgpu" }
   "none"
}

fn accel_target_available(any target="") bool {
   "Returns true when runtime or toolchain for target appears available."
   def t = _resolve_accel_target(target)
   _target_runtime_available(t) || _target_toolchain_available(t)
}

fn accel_target_status(any target="") dict {
   "Returns accelerator target status map including availability and artifact details."
   def configured = ACCEL_TARGET
   def selected = _resolve_accel_target(target)
   def runtime_ok = _target_runtime_available(selected)
   def toolchain_ok = _target_toolchain_available(selected)
   def available = runtime_ok || toolchain_ok
   mut reason = "none_selected"
   if selected != "none" {
      if runtime_ok && toolchain_ok { reason = "ready_runtime_and_toolchain" }
      elif toolchain_ok { reason = "toolchain_only" }
      elif runtime_ok { reason = "runtime_only" }
      else { reason = "runtime_and_toolchain_missing" }
   }
   {"configured_target": configured, "selected_target": selected, "triple": accel_target_triple(selected),
      "object_kind": accel_binary_kind(selected), "object_ext": accel_binary_ext(selected),
      "runtime_available": runtime_ok, "toolchain_available": toolchain_ok, "available": available,
   "gpu_backend": GPU_BACKEND, "gpu_available": GPU_AVAILABLE, "reason": reason}
}

fn accel_compile_plan(str input_path, any output_path="", any target="") dict {
   "Returns a best-effort device compilation plan map for selected accelerator target."
   def t = _resolve_accel_target(target)
   def triple = accel_target_triple(t)
   def kind = accel_binary_kind(t)
   def ext = accel_binary_ext(t)
   mut out_path = output_path
   if !is_str(out_path) || strip(out_path).len == 0 { out_path = "device" + ext }
   def cc = _env_str_or("NYTRIX_ACCEL_CLANG", "clang")
   def spv_tool = _env_str_or("NYTRIX_ACCEL_LLVM_SPIRV", "llvm-spirv")
   def opt = _env_str_or("NYTRIX_ACCEL_OPT", "3")
   def nv_arch = _env_str_or("NYTRIX_ACCEL_ARCH_NVPTX", "sm_80")
   def amd_arch = _env_str_or("NYTRIX_ACCEL_ARCH_AMDGPU", "gfx1100")
   mut cmd = list(0)
   if t == "nvptx" {
      cmd = [cc, "-target", "nvptx64-nvidia-cuda", "--cuda-gpu-arch=" + nv_arch, "-O" + opt, "-S", input_path, "-o", out_path]
   } elif t == "amdgpu" {
      cmd = [cc, "-target", "amdgcn-amd-amdhsa", "-mcpu=" + amd_arch, "-O" + opt, "-c", input_path, "-o", out_path]
   } elif t == "spirv" {
      if endswith(lower(input_path), ".bc") {
         cmd = [spv_tool, input_path, "-o", out_path]
      } else {
         cmd = [cc, "-target", "spirv64-unknown-unknown", "-O" + opt, "-c", input_path, "-o", out_path]
      }
   } elif t == "hsaco" {
      cmd = [cc, "-target", "amdgcn-amd-amdhsa", "--offload-arch=" + amd_arch, "-O" + opt, "-c", input_path, "-o", out_path]
   }
   {"target": t, "triple": triple, "object_kind": kind, "object_ext": ext, "input": input_path,
   "output": out_path, "command": cmd, "status": accel_target_status(t)}
}

fn accel_emit_plan(any function_name, str ir_path, any out_dir="", any target="") dict {
   "Returns formal @accel device-emission metadata for one marked function."
   def t = _resolve_accel_target(target)
   def ext = accel_binary_ext(t)
   mut name = to_str(function_name)
   if name.len == 0 { name = "device" }
   name = str.str_replace(name, ".", "_")
   name = str.str_replace(name, ":", "_")
   mut output = name + ext
   if is_str(out_dir) && strip(out_dir).len > 0 { output = out_dir + "/" + output }
   def plan = accel_compile_plan(ir_path, output, t)
   plan.merge({"function": function_name, "backend": accel_backend(t), "attribute": "@accel",
   "emits_device_artifact": plan.get("command", list()).len > 0})
}

fn accel_emit_command(any function_name, str ir_path, any out_dir="", any target="") list {
   "Returns the command vector from accel_emit_plan(...)."
   accel_emit_plan(function_name, ir_path, out_dir, target).get("command", list())
}

fn _opencl_cpu_threads_guess() int {
   def n1 = common.parse_nonneg_int(env("NYTRIX_LOGICAL_CPUS"))
   if n1 > 0 { return n1 }
   def n2 = common.parse_nonneg_int(env("NUMBER_OF_PROCESSORS"))
   if n2 > 0 { return n2 }
   def n3 = common.parse_nonneg_int(env("NPROC"))
   if n3 > 0 { return n3 }
   1
}

fn opencl_available() bool {
   "Returns true when an OpenCL runtime appears available."
   accel_target_status("spirv").get("runtime_available", false)
}

fn opencl_toolchain_available() bool {
   "Returns true when a SPIR-V/OpenCL-capable toolchain appears available."
   accel_target_status("spirv").get("toolchain_available", false)
}

fn opencl_async() bool {
   "Returns the configured async GPU dispatch preference."
   gpu_async()
}

fn opencl_fast_math() bool {
   "Returns the configured relaxed math preference for accelerator code."
   gpu_fast_math()
}

fn opencl_work_groups(int global_size, int local_size=0) int {
   "Returns the number of work groups for `global_size` and optional `local_size`."
   if global_size <= 0 { return 0 }
   if local_size <= 0 { return 1 }
   (global_size + local_size - 1) / local_size
}

fn opencl_status(int work_items=0) dict {
   "Returns OpenCL policy, availability, and CPU fallback metadata."
   def target = accel_target_status("spirv")
   def gpu_st = gpu_offload_status(work_items)
   def mode = gpu_mode()
   def backend = gpu_backend()
   def offload = gpu_offload()
   def runtime_ok = target.get("runtime_available", false)
   def toolchain_ok = target.get("toolchain_available", false)
   def min_work = gpu_st.get("effective_min_work", 4096)
   mut selected = false
   mut reason = "cpu_default"
   if mode == "off" { reason = "gpu_mode_off" } elif backend != "auto" && backend != "opencl" {
      reason = "gpu_backend_not_opencl"
   } elif offload == "off" {
      reason = "offload_mode_off"
   } elif !runtime_ok && offload != "force" {
      reason = "opencl_runtime_unavailable"
   } elif offload == "force" {
      if runtime_ok {
         selected = true
         reason = "forced"
      } else {
         reason = "forced_but_opencl_unavailable"
      }
   } elif work_items > 0 && work_items < min_work {
      reason = "below_min_work"
   } else {
      selected = runtime_ok
      if selected { reason = "eligible" } else { reason = "opencl_runtime_unavailable" }
   }
   mut active_reason = "policy_not_selected"
   if selected { active_reason = "runtime_backend_unimplemented" }
   {"backend": "opencl", "target": "spirv", "mode": mode, "configured_backend": backend, "offload": offload,
      "runtime_available": runtime_ok, "toolchain_available": toolchain_ok, "available": runtime_ok,
      "work_items": work_items, "effective_min_work": min_work, "async": gpu_async(), "fast_math": gpu_fast_math(),
      "policy_selected": selected, "active": false, "reason": reason, "active_reason": active_reason,
      "cpu_threads": _opencl_cpu_threads_guess(), "cpu_parallel_selected": false,
   "cpu_parallel_reason": "gpu_module_no_parallel_import"}
}

fn opencl_should_offload(int work_items=0) bool {
   "Returns true when OpenCL policy selects device execution."
   opencl_status(work_items).get("policy_selected", false)
}

fn opencl_device_policy(int work_items=0) dict {
   "Alias for opencl_status; useful at call sites that choose CPU/GPU plans."
   opencl_status(work_items)
}

fn opencl_compile_plan(str input_path, any output_path="") dict {
   "Returns a SPIR-V compile command plan for an LLVM IR input."
   accel_compile_plan(input_path, output_path, "spirv")
}

fn opencl_kernel_plan(any name, int global_size, int local_size=0) dict {
   "Returns normalized launch-shape metadata for an OpenCL-style kernel."
   opencl_status(global_size).merge({"kernel": name, "global_size": global_size, "local_size": local_size,
   "work_groups": opencl_work_groups(global_size, local_size)})
}

fn opencl_cpu_fallback_plan(int work_items=0, int item_cost=1) dict {
   "Returns the CPU plan used when OpenCL is unavailable or not selected."
   def st = opencl_status(work_items)
   mut threads = st.get("cpu_threads", 1)
   if threads < 1 { threads = 1 }
   mut chunk = 0
   if work_items > 0 { chunk = (work_items + threads - 1) / threads }
   {"backend": "cpu", "work_items": work_items, "threads": threads, "chunk_size": chunk, "item_cost": item_cost,
      "gpu_policy_selected": st.get("policy_selected", false), "gpu_active": st.get("active", false),
   "reason": st.get("active_reason", st.get("reason", "policy_not_selected"))}
}

fn opencl_dispatch_plan(any name, int global_size, int local_size=0) dict {
   "Returns the full OpenCL kernel plan plus the CPU fallback plan."
   mut out = opencl_kernel_plan(name, global_size, local_size)
   def fallback = opencl_cpu_fallback_plan(global_size)
   out.merge({"fallback": fallback, "dispatch_backend": out.get("active", false) ? "opencl" : "cpu"})
}

def OPENCL_AVAILABLE = opencl_available()
def OPENCL_TOOLCHAIN_AVAILABLE = opencl_toolchain_available()

#main {
   assert(is_str(gpu_mode()) && gpu_mode().len > 0 && is_str(gpu_backend()) && gpu_backend().len > 0 && is_str(gpu_offload()) && gpu_offload().len > 0, "gpu config")
   assert(gpu_min_work() >= 0 && is_bool(gpu_async()) && is_bool(gpu_fast_math()) && is_bool(gpu_available()), "gpu flags")
   assert(is_dict(gpu_offload_status(64)), "gpu offload status")
   assert(is_str(accel_target()) && accel_target().len > 0 && is_dict(accel_target_status(accel_target())), "gpu accel target")
   def st = opencl_status(1024)
   assert(is_dict(st) && st.get("backend", "") == "opencl" && st.get("target", "") == "spirv" && st.get("cpu_threads", 0) >= 1, "opencl status")
   assert(is_bool(opencl_available()) && is_bool(opencl_toolchain_available()), "opencl availability")
   assert(opencl_work_groups(1025, 256) == 5, "opencl work groups")
   def kernel = opencl_kernel_plan("scan", 1025, 256)
   assert(kernel.get("kernel", "") == "scan" && kernel.get("work_groups", 0) == 5, "opencl kernel plan")
   def fallback = opencl_cpu_fallback_plan(1025, 2)
   assert(fallback.get("backend", "") == "cpu" && fallback.get("threads", 0) >= 1, "opencl fallback")
   def dispatch = opencl_dispatch_plan("scan", 1025, 256)
   assert((dispatch.get("dispatch_backend", "") == "cpu" || dispatch.get("dispatch_backend", "") == "opencl") && is_dict(dispatch.get("fallback")), "opencl dispatch")
   assert(is_dict(opencl_compile_plan("kernel.ll")), "opencl compile plan")
   print("✓ std.os.gpu self-test passed")
}
