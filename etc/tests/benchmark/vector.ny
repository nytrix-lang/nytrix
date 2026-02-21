use std.core *
use std.str.io *
use std.os *
use std.os.time *
use std.math.vector as v
use std.core.dict *

;; Vector Benchmark (Merged: Vanilla + Offload)
;; Evaluates hadamard product performance with adaptive workload.

def n = 8192
mut rounds = 128
mut a = list(n)
mut b = list(n)
mut i = 0
while(i < n){
   a = append(a, i + 1)
   b = append(b, i + 3)
   i += 1
}

def st_raw = gpu_offload_status(n * rounds)
mut st = st_raw
if(!is_dict(st_raw)){
   st = dict(8)
   st = dict_set(st, "reason", "status_unavailable")
   st = dict_set(st, "selected_backend", gpu_backend())
   st = dict_set(st, "active", false)
}

;; Adjust rounds for slow/constrained platforms if offload is not active
if(dict_get(st, "active", false) == false){
   rounds = 24
   if(os() == "macos" || os() == "windows"){ rounds = 12 }
}

print("GPU policy:", dict_get(st, "reason", ""), "backend:", dict_get(st, "selected_backend", "none"), "active:", dict_get(st, "active", false))
print("Running", rounds, "rounds of size", n)

def t0 = ticks()
mut acc = 0
mut r = 0
while(r < rounds){
   def c = v.hadamard(a, b)
   acc = acc + get(c, 0, 0) + get(c, n - 1, 0)
   r += 1
}
def t1 = ticks()

assert(acc > 0, "vector bench acc")
print("Vector benchmark acc =", acc)
print("Time (ns): ", t1 - t0)
