use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *

;; Sieve of Eratosthenes (Benchmark)

fn sieve(n){
   if(n < 2){ return 0 }
   def size = (n >> 1) + 1
   def flags = malloc(size)
   mut i = 0
   while(i < size){
      store8(flags, 1, i)
      i += 1
   }
   mut count = 1
   mut p = 3
   while(p * p <= n){
      if(load8(flags, p >> 1)){
         def sq = p * p
         mut mul = sq
         while(mul <= n){
            store8(flags, 0, mul >> 1)
            mul += p * 2
         }
      }
      p += 2
   }
   i = p
   while(i <= n){
      if(load8(flags, i >> 1)){ count += 1 }
      i += 2
   }
   free(flags)
   return count
}

def N = 10000000
print(f"Benchmarking Sieve up to {to_str(N)}")

def t0 = ticks()
def r = sieve(N)
def t1 = ticks()

print(f"Primes: {to_str(r)}")
print(f"Time: {to_str((t1 - t0) / 1000000)} ms")
