use std.core
use std.os.time

;; Vector Benchmark (Benchmark)
; Fixed scale values (helpers may return 0 if env not set)
fn hadamard_kernel(a, b){
   def n = a.len
   mut out = list(n)
   mut i = 0
   while(i < n){
      __store_item_fast(out, i, __load_item_fast(a, i) * __load_item_fast(b, i))
      i += 1
   }
   __list_set_len(out, n)
   out
}

def n = 1024
def rounds = 2
mut a = list(n)
mut b = list(n)
mut i = 0
while(i < n){
   a = a.append(i + 1)
   b = b.append(i + 3)
   i += 1
}

print("Running", rounds, "rounds of size", n)
def t0 = ticks()
mut acc = 0
mut r = 0
while(r < rounds){
   def c = hadamard_kernel(a, b)
   acc = acc + c.get(0, 0) + c.get(n - 1, 0)
   r += 1
}

def t1 = ticks()
print("Vector benchmark acc =", acc)
print("Time(ns): ", t1 - t0)
