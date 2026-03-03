use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *
use std.os *
use std.os.time *

;; Float (Benchmark)

use std.core.iter *

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

def start = ticks()
mut x = 1.0
def y = 1.000001
def iters = _bench_scale(500000, 1000)
for(_ in range(0, iters)){
   x = x * y
}

def end = ticks()
def dur = end - start
print("Float ", iters, " muls took (ns): ", dur)
print("Result: ", x)
print("Time (ns): ", dur)
