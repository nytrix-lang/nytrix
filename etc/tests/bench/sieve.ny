use std.io
use std.os.time

;; Sieve of Eratosthenes (Optimized Benchmark)
;; Odd-only sieve, byte flags.

fn sieve(n){
   if(n < 2){ return 0 }

   ; only odds: index = x >> 1
   def size = (n >> 1) + 1
   def flags = __malloc(size)

   def i = 0
   while(i < size){
      store8(flags, 1, i)
      i += 1
   }

   def count = 1 ; prime = 2

   def p = 3
   while(p * p <= n){
      if(load8(flags, p >> 1)){
         def j = p * p
         def step = p << 1
         while(j <= n){
            store8(flags, 0, j >> 1)
            j += step
         }
      }
      p += 2
   }

   i = 3
   while(i <= n){
      if(load8(flags, i >> 1)){ count += 1 }
      i += 2
   }

   __free(flags)
   return count
}

def N = 10000000
print(f"Benchmarking Sieve up to {to_str(N)}")

def t0 = ticks()
def r = sieve(N)
def t1 = ticks()

print(f"Primes: {to_str(r)}")
print(f"Time: {to_str((t1 - t0) / 1000000)} ms")
