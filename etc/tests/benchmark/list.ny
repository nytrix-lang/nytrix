use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.text.io *
use std.text *
use std.os *
use std.os.time *

;; List (Benchmark)

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
mut lst = []
mut idx = 0
def n = _bench_scale(1000, 200)
while(idx < n){
   lst = append(lst, idx)
   idx = idx + 1
}
assert(len(lst) == n, "list size")

mut sum = 0
idx = 0
while(idx < len(lst)){
   sum = sum + get(lst, idx)
   idx = idx + 1
}
def end = ticks()

def expected = (n - 1) * n / 2 ; Sum of 0..n-1
assert(sum == expected, "list sum")
print("List benchmark passed, sum =", sum)
print("Time (ns): ", end - start)
