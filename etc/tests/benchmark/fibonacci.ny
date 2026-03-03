use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *
use std.os *
use std.os.time *

;; Fibonacci (Benchmark)

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

fn fib(n){
   mut a = 0
   mut b = 1
   mut i = 2
   while(i <= n){
      def c = a + b
      a = b
      b = c
      i += 1
   }
   return b
}

def iters = _bench_scale(50000, 500)
mut i = 0
mut r = 0

def t0 = ticks()
while(i < iters){
   r = fib(40)
   i += 1
}
def t1 = ticks()

def elapsed_ms = (t1 - t0) / 1000000
print("Fib(40) = ", r)
print("Avg Time: ", to_str(elapsed_ms / iters), " ms")
