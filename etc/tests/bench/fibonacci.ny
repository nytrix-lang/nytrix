use std.io
use std.os.time
use std.strings.str

;; Fibonacci (Benchmark)
;; Tests recursion performance and integer arithmetic.

fn fib(n){
   def a = 0
   def b = 1
   def i = 2
   while(i <= n){
      def c = a + b
      a = b
      b = c
      i += 1
   }
   return b
}

def iters = 100000
def i = 0
def r = 0

def t0 = ticks()
while(i < iters){
   r = fib(40)
   i += 1
}
def t1 = ticks()

def elapsed_ms = (t1 - t0) / 1000000
print("Fib(40) = ", r)
print("Avg Time: ", to_str(elapsed_ms / iters), " ms")
