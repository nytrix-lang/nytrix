use std.core
use std.math.float (float)
use std.os.time
use benchmark.helpers

;; Spectral Norm (Benchmark)
def N = _bench_scale(56, 16)
def ITERS = _bench_scale(2, 1)
mut u = list(N)
mut v = list(N)
mut tmp = list(N)
mut i = 0
while(i < N){
   u = u.append(1.0)
   v = v.append(0.0)
   tmp = tmp.append(0.0)
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
         sum += A(i, j) * x.get(j)
         j += 1
      }
      out.set(i, sum)
      i += 1
   }
}

fn mul_Atv(x, out){
   mut i = 0
   while(i < N){
      mut sum = 0.0
      mut j = 0
      while(j < N){
         sum += A(j, i) * x.get(j)
         j += 1
      }
      out.set(i, sum)
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
   vbv += u.get(i) * v.get(i)
   vv += v.get(i) * v.get(i)
   i += 1
}

def res = __flt_sqrt(float(vbv / vv))
def end = ticks()
print("Result:", res)
print("Time:", (end - start) / 1000000, "ms")
