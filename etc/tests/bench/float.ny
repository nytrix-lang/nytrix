use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *

;; Float (Benchmark)

def start = ticks()
mut x = 1.0
def y = 1.000001
mut i = 0
while(i < 1000000){
   x = x * y
   i = i + 1
}

def end = ticks()
def dur = end - start
print("Float 1M muls took (ns): ", dur)
print("Result: ", x)
print("Time (ns): ", dur)
