use std.core
use std.core.iter as it
use std.os.time
use benchmark.helpers

def n = _bench_scale(4000, 500)
mut xs = list(n)
mut i = 0
while(i < n){
   store_item(xs, i, i)
   i += 1
}

store64(xs, n, 0)
def start = ticks()
def mapped = it.map(xs, fn(v){ v + 1 })
def reversed = it.reverse(mapped)
def chained = it.chain(mapped, reversed)
mut sum = 0
i = 0
while(i < len(chained)){
   sum += chained.get(i)
   i += 1
}

def end = ticks()
assert(len(mapped) == n, "iter map size")
assert(len(reversed) == n, "iter reverse size")
assert(len(chained) == len(mapped) + len(reversed), "iter chain size")
print("Iter benchmark passed, sum =", sum)
print("Time(ns): ", end - start)
