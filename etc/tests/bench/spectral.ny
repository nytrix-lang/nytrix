use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *

;; Spectral Norm (Benchmark)

def N = 64
def ITERS = 3

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

