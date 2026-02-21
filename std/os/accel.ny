;; Keywords: os accel gpu parallel
;; Accelerator policy facade.

module std.os.accel (
   gpu_mode, gpu_backend, gpu_offload, gpu_min_work, gpu_async, gpu_fast_math,
   gpu_available, gpu_should_offload, gpu_offload_status,
   accel_target, accel_targets, accel_target_available, accel_target_triple, accel_binary_kind,
   accel_binary_ext, accel_target_status, accel_compile_plan,
   parallel_mode, parallel_threads, parallel_min_work, parallel_should_threads, parallel_status,
   GPU_MODE, GPU_BACKEND, GPU_OFFLOAD, GPU_MIN_WORK, GPU_ASYNC, GPU_FAST_MATH, GPU_AVAILABLE,
   ACCEL_TARGET, ACCEL_OBJECT,
   PARALLEL_MODE, PARALLEL_THREADS, PARALLEL_MIN_WORK
)

use std.os (
   gpu_mode as _os_gpu_mode,
   gpu_backend as _os_gpu_backend,
   gpu_offload as _os_gpu_offload,
   gpu_min_work as _os_gpu_min_work,
   gpu_async as _os_gpu_async,
   gpu_fast_math as _os_gpu_fast_math,
   gpu_available as _os_gpu_available,
   gpu_should_offload as _os_gpu_should_offload,
   gpu_offload_status as _os_gpu_offload_status,
   accel_target as _os_accel_target,
   accel_targets as _os_accel_targets,
   accel_target_available as _os_accel_target_available,
   accel_target_triple as _os_accel_target_triple,
   accel_binary_kind as _os_accel_binary_kind,
   accel_binary_ext as _os_accel_binary_ext,
   accel_target_status as _os_accel_target_status,
   accel_compile_plan as _os_accel_compile_plan,
   parallel_mode as _os_parallel_mode,
   parallel_threads as _os_parallel_threads,
   parallel_min_work as _os_parallel_min_work,
   parallel_should_threads as _os_parallel_should_threads,
   parallel_status as _os_parallel_status
)

fn gpu_mode(){
   "Returns the configured GPU mode: `off`, `auto`, or `opencl`."
   _os_gpu_mode()
}

fn gpu_backend(){
   "Returns configured GPU backend: `none`, `auto`, `opencl`, `cuda`, `hip`, or `metal`."
   _os_gpu_backend()
}

fn gpu_offload(){
   "Returns GPU offload policy: `off`, `auto`, `on`, or `force`."
   _os_gpu_offload()
}

fn gpu_min_work(){
   "Returns minimum work threshold before trying GPU offload; `0` means auto/default."
   _os_gpu_min_work()
}

fn gpu_async(){
   "Returns true when async GPU dispatch is enabled."
   _os_gpu_async()
}

fn gpu_fast_math(){
   "Returns true when relaxed GPU math optimizations are enabled."
   _os_gpu_fast_math()
}

fn gpu_available(){
   "Returns true when the selected GPU backend appears available on this host."
   _os_gpu_available()
}

fn gpu_offload_status(work_items=0){
   "Returns an offload decision map for `work_items`."
   _os_gpu_offload_status(work_items)
}

fn gpu_should_offload(work_items=0){
   "Returns true when offload policy selects GPU for `work_items`."
   _os_gpu_should_offload(work_items)
}

fn accel_target(){
   "Returns the selected accelerator target: `none|nvptx|amdgpu|spirv|hsaco`."
   _os_accel_target()
}

fn accel_targets(){
   "Returns canonical accelerator targets ordered by host preference."
   _os_accel_targets()
}

fn accel_target_available(target=""){
   "Returns true when runtime or toolchain for target appears available."
   _os_accel_target_available(target)
}

fn accel_target_triple(target=""){
   "Returns LLVM-style triple for selected accelerator target."
   _os_accel_target_triple(target)
}

fn accel_binary_kind(target=""){
   "Returns emitted device artifact kind: `ptx|o|spv|hsaco|none`."
   _os_accel_binary_kind(target)
}

fn accel_binary_ext(target=""){
   "Returns suggested file extension for emitted device artifact."
   _os_accel_binary_ext(target)
}

fn accel_target_status(target=""){
   "Returns accelerator target status map."
   _os_accel_target_status(target)
}

fn accel_compile_plan(input_path, output_path="", target=""){
   "Returns best-effort device compilation command plan for chosen target."
   _os_accel_compile_plan(input_path, output_path, target)
}

fn parallel_mode(){
   "Returns the configured parallel mode: `off`, `auto`, or `threads`."
   _os_parallel_mode()
}

fn parallel_threads(){
   "Returns configured thread budget; `0` means runtime/default auto sizing."
   _os_parallel_threads()
}

fn parallel_min_work(){
   "Returns minimum work threshold before selecting threaded parallel execution."
   _os_parallel_min_work()
}

fn parallel_status(work_items=0){
   "Returns a threading decision map for `work_items`."
   _os_parallel_status(work_items)
}

fn parallel_should_threads(work_items=0){
   "Returns true when thread-parallel policy selects threaded execution."
   _os_parallel_should_threads(work_items)
}

def GPU_MODE = gpu_mode()
def GPU_BACKEND = gpu_backend()
def GPU_OFFLOAD = gpu_offload()
def GPU_MIN_WORK = gpu_min_work()
def GPU_ASYNC = gpu_async()
def GPU_FAST_MATH = gpu_fast_math()
def GPU_AVAILABLE = gpu_available()
def ACCEL_TARGET = accel_target()
def ACCEL_OBJECT = accel_binary_kind(ACCEL_TARGET)
def PARALLEL_MODE = parallel_mode()
def PARALLEL_THREADS = parallel_threads()
def PARALLEL_MIN_WORK = parallel_min_work()

if(comptime{__main()}){
    use std.os.accel *
    use std.core.error *
    use std.core.dict *
    use std.core *
    use std.str *

    fn _contains(xs, x){
       "Test helper."
       mut i = 0
       while(i < len(xs)){
          if(get(xs, i, "") == x){ return true }
          i += 1
       }
       false
    }

    print("Testing std.os.accel...")

    def gm = gpu_mode()
    def gb = gpu_backend()
    def go = gpu_offload()
    def gw = gpu_min_work()
    def ga = gpu_async()
    def gfm = gpu_fast_math()
    def gav = gpu_available()
    def at = accel_target()
    def ats = accel_targets()
    def atav = accel_target_available()
    def atr = accel_target_triple()
    def abk = accel_binary_kind()
    def abe = accel_binary_ext()
    def atst = accel_target_status()
    def apl = accel_compile_plan("kernel.ll")
    def pm = parallel_mode()
    def tn = parallel_threads()
    def pmin = parallel_min_work()
    def st_small = gpu_offload_status(64)
    def st_big = gpu_offload_status(1000000)
    def pst_small = parallel_status(64)
    def pst_big = parallel_status(1000000)

    assert((eq(gm, "off") || eq(gm, "auto") || eq(gm, "opencl")), "gpu_mode value")
    assert((eq(gb, "none") || eq(gb, "auto") || eq(gb, "opencl") || eq(gb, "cuda") || eq(gb, "hip") || eq(gb, "metal")), "gpu_backend value")
    assert((eq(go, "off") || eq(go, "auto") || eq(go, "on") || eq(go, "force")), "gpu_offload value")
    assert(gw >= 0, "gpu_min_work non-negative")
    assert((ga == true || ga == false), "gpu_async bool")
    assert((gfm == true || gfm == false), "gpu_fast_math bool")
    assert((gav == true || gav == false), "gpu_available bool")
    assert((eq(at, "none") || eq(at, "nvptx") || eq(at, "amdgpu") || eq(at, "spirv") || eq(at, "hsaco")), "accel_target value")
    assert(is_list(ats), "accel_targets list")
    assert(len(ats) >= 4, "accel_targets size")
    assert(_contains(ats, "nvptx"), "accel_targets has nvptx")
    assert(_contains(ats, "amdgpu"), "accel_targets has amdgpu")
    assert(_contains(ats, "spirv"), "accel_targets has spirv")
    assert(_contains(ats, "hsaco"), "accel_targets has hsaco")
    assert((atav == true || atav == false), "accel_target_available bool")
    assert((eq(atr, "none") || eq(atr, "nvptx64-nvidia-cuda") || eq(atr, "amdgcn-amd-amdhsa") || eq(atr, "spirv64-unknown-unknown")), "accel_target triple value")
    assert((eq(abk, "none") || eq(abk, "ptx") || eq(abk, "o") || eq(abk, "spv") || eq(abk, "hsaco")), "accel_binary_kind value")
    assert((eq(abe, "") || eq(abe, ".ptx") || eq(abe, ".o") || eq(abe, ".spv") || eq(abe, ".hsaco")), "accel_binary_ext value")
    assert(is_dict(atst), "accel_target_status dict")
    assert(str_len(dict_get(atst, "selected_target", "")) > 0, "accel_target_status selected_target")
    assert((dict_get(atst, "available", false) == true || dict_get(atst, "available", false) == false), "accel_target_status available")
    assert(str_len(dict_get(atst, "reason", "")) > 0, "accel_target_status reason")
    assert(is_dict(apl), "accel_compile_plan dict")
    assert((dict_get(apl, "target", "") == at), "accel_compile_plan target")
    assert((dict_get(apl, "object_kind", "") == abk), "accel_compile_plan object_kind")
    def apc = dict_get(apl, "command", list(1))
    assert(is_list(apc), "accel_compile_plan command list")
    if(at != "none"){ assert(len(apc) >= 6, "accel_compile_plan command non-empty for active target") }
    assert((eq(pm, "off") || eq(pm, "auto") || eq(pm, "threads")), "parallel_mode value")
    assert(tn >= 0, "parallel_threads non-negative")
    assert(pmin >= 0, "parallel_min_work non-negative")
    assert(is_dict(st_small), "gpu_offload_status small dict")
    assert(is_dict(st_big), "gpu_offload_status big dict")
    assert((dict_get(st_small, "available", false) == true || dict_get(st_small, "available", false) == false), "gpu_offload_status available bool")
    assert((dict_get(st_small, "policy_selected", false) == true || dict_get(st_small, "policy_selected", false) == false), "gpu_offload_status policy bool")
    assert((dict_get(st_small, "active", false) == true || dict_get(st_small, "active", false) == false), "gpu_offload_status active bool")
    assert(str_len(dict_get(st_small, "reason", "")) > 0, "gpu_offload_status reason")
    assert(str_len(dict_get(st_small, "active_reason", "")) > 0, "gpu_offload_status active reason")
    assert((gpu_should_offload(64) == true || gpu_should_offload(64) == false), "gpu_should_offload bool")
    assert((gpu_should_offload(1000000) == true || gpu_should_offload(1000000) == false), "gpu_should_offload bool big")
    assert(is_dict(pst_small), "parallel_status small dict")
    assert(is_dict(pst_big), "parallel_status big dict")
    assert((dict_get(pst_small, "selected", false) == true || dict_get(pst_small, "selected", false) == false), "parallel_status selected bool")
    assert(str_len(dict_get(pst_small, "reason", "")) > 0, "parallel_status reason")
    assert(dict_get(pst_small, "effective_threads", 0) >= 1, "parallel_status effective_threads")
    assert((parallel_should_threads(64) == true || parallel_should_threads(64) == false), "parallel_should_threads bool")
    assert((parallel_should_threads(1000000) == true || parallel_should_threads(1000000) == false), "parallel_should_threads bool big")

    print("✓ std.os.accel tests passed")
}
