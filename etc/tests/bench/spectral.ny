use std.io
use std.os.time
use std.math.float

;; Spectral Norm (Benchmark)
;; Tests math performance and array access patterns.

def N = 64
def ITERS = 3

def u = list(N)
def v = list(N)
def tmp = list(N)

def i = 0
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
   def i = 0
   while(i < N){
      def sum = 0.0
      def j = 0
      while(j < N){
         sum += A(i, j) * get(x, j)
         j += 1
      }
      set_idx(out, i, sum)
      i += 1
   }
}

fn mul_Atv(x, out){
   def i = 0
   while(i < N){
      def sum = 0.0
      def j = 0
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

def vbv = 0.0
def vv = 0.0
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
