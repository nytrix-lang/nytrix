use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.text.io *
use std.text *
use std.os *
use std.os.time *

;; Spectral Norm (Benchmark)

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

def N = _bench_scale(56, 16)
def ITERS = _bench_scale(2, 1)

mut u = list(N)
mut v = list(N)
mut tmp = list(N)

mut i = 0
while(i < N){
   u = append(u, 1.0)
   v = append(v, 0.0)
   tmp = append(tmp, 0.0)
   i += 1
}

fn A(i, j){
   def ij = i + j
   return 1.0 / (((ij * (ij + 1)) >> 1) + i + 1)
}

fn mul_Av(x, out){
   mut i = 0
   while(i < N){
      mut sum = 0.0
      mut j = 0
      while(j < N){
         sum += A(i, j) * get(x, j)
         j += 1
      }
      set_idx(out, i, sum)
      i += 1
   }
}

fn mul_Atv(x, out){
   mut i = 0
   while(i < N){
      mut sum = 0.0
      mut j = 0
      while(j < N){
         sum += A(j, i) * get(x, j)
         j += 1
      }
      set_idx(out, i, sum)
      i += 1
   }
}

def start = ticks()

i = 0
while(i < ITERS){
   mul_Av(u, tmp)
   mul_Atv(tmp, v)
   mul_Av(v, tmp)
   mul_Atv(tmp, u)
   i += 1
}

mut vbv = 0.0
mut vv = 0.0
i = 0
while(i < N){
   vbv += get(u, i) * get(v, i)
   vv += get(v, i) * get(v, i)
   i += 1
}

def res = sqrt(vbv / vv)
def end = ticks()

print("Result:", res)
print("Time:", (end - start) / 1000000, "ms")
