use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.text.io *
use std.text *
use std.os.time *

;; Float (Benchmark)

use std.core.iter *

def start = ticks()
mut x = 1.0
def y = 1.000001
def iters = 500000
for(_ in range(0, iters)){
   x = x * y
}

def end = ticks()
def dur = end - start
print("Float ", iters, " muls took (ns): ", dur)
print("Result: ", x)
print("Time (ns): ", dur)
