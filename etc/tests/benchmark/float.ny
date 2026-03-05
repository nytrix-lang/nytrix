use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *
use std.os *
use std.os.time *
use benchmark.helpers *

;; Float (Benchmark)

use std.core.iter *

def start = ticks()
mut x = 1.0
def y = 1.000001
def iters = _bench_scale(500000, 1000)
for(_ in range(0, iters)){
   x = x * y
}

def end = ticks()
def dur = end - start
print("Float ", iters, " muls took (ns): ", dur)
print("Result: ", x)
print("Time (ns): ", dur)
