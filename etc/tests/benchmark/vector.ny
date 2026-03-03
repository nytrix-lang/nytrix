use std.core *
use std.str.io *
use std.os *
use std.os.time *
use std.math.vector as v
use std.core.dict *

;; Vector Benchmark (Benchmark)

fn _bench_scale_percent(){
   def raw = env("NYTRIX_BENCH_SCALE")
   if(is_str(raw) && str_len(raw) > 0){
      def v = atoi(raw)
      if(v > 0){ return v }
   }
   100
}

fn _bench_scale(val, minv){
   mut out = (val * _bench_scale_percent()) / 100
   if(out < minv){ out = minv }
   out
}

def n = _bench_scale(8192, 1024)
mut rounds = _bench_scale(128, 4)
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

if(dict_get(st, "active", false) == false){
   rounds = _bench_scale(24, 2)
   if(os() == "macos" || os() == "windows"){ rounds = _bench_scale(12, 1) }
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
