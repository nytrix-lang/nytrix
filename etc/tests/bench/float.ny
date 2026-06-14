use std.core
use std.os.time
use benchmark.helpers

;; Float (Benchmark)
def start = ticks()
mut x = 1.0
def y = 1.000001
def iters = _bench_scale(500000, 1000)
mut i = 0
while i < iters {
   x = x * y
   i += 1
}

def end = ticks()
def dur = end - start
print("Float ", iters, " muls took(ns): ", dur)
print("Result: ", x)
print("Time(ns): ", dur)
