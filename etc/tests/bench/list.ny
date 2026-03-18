use std.core
use std.os.time
use benchmark.helpers

;; List (Benchmark)
def start = ticks()
mut lst = []
mut idx = 0
def n = _bench_scale(1000, 200)
while(idx < n){
   lst = lst.append(idx)
   idx = idx + 1
}

assert(len(lst) == n, "list size")
mut sum = 0
idx = 0
while(idx < len(lst)){
   sum = sum + lst.get(idx)
   idx = idx + 1
}

def end = ticks()
def expected = (n - 1) * n / 2 ; Sum of 0..n-1
assert(sum == expected, "list sum")
print("List benchmark passed, sum =", sum)
print("Time(ns): ", end - start)
