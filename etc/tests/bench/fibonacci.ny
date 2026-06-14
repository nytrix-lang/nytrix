use std.core
use std.os.time
use benchmark.helpers

;; Fibonacci (Benchmark)
fn fib(n) {
   mut a = 0
   mut b = 1
   mut i = 2
   while i <= n {
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
while i < iters {
   r = fib(40)
   i += 1
}

def t1 = ticks()
def elapsed_ms = (t1 - t0) / 1000000
print("Fib(40) = ", r)
print("Avg Time: ", to_str(elapsed_ms / iters), " ms")
