use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *
use std.os.time *

;; List (Benchmark)

def start = ticks()
mut lst = []
mut idx = 0
while(idx < 1000){
   lst = append(lst, idx)
   idx = idx + 1
}
assert(len(lst) == 1000, "list size")

mut sum = 0
idx = 0
while(idx < len(lst)){
   sum = sum + get(lst, idx)
   idx = idx + 1
}
def end = ticks()

def expected = 499500 ; Sum of 0..999
assert(sum == expected, "list sum")
print("List benchmark passed, sum =", sum)
print("Time (ns): ", end - start)
