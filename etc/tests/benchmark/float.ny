use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *
use std.os.time *

;; Float (Benchmark)

def start = ticks()
mut x = 1.0
def y = 1.000001
mut i = 0
def iters = 500000
while(i < iters){
   x = x * y
   i = i + 1
}

def end = ticks()
def dur = end - start
print("Float ", iters, " muls took (ns): ", dur)
print("Result: ", x)
print("Time (ns): ", dur)
