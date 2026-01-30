use std.core *
use std.core.error *
use std.core.reflect *
use std.core.dict *
use std.str.io *
use std.str.io *
use std.str *

;; Binary Trees (Benchmark)

fn make_tree(depth){
   if(depth == 0){ return 0 }
   mut d = depth - 1
   return [make_tree(d), make_tree(d)]
}

fn check(t){
   if(t == 0){ return 1 }
   return 1 + check(get(t,0)) + check(get(t,1))
}

def min_depth = 4
def max_depth = 10

print("Benchmarking Binary Trees (fast mode, max depth ", max_depth, ")")

def start = ticks()

mut trees = dict()
mut d = min_depth
while(d <= max_depth){
   trees = dict_set(trees, d, make_tree(d))
   d += 2
}

def stretch = make_tree(max_depth + 1)
print(
   "Stretch tree of depth ",
   max_depth + 1,
   " check: ",
   check(stretch)
)

d = min_depth
while(d <= max_depth){
   def iterations = 1 << (max_depth - d)
   def t = dict_get(trees, d, 0)
   def chk = check(t) * iterations
   print(iterations, " trees of depth ", d, " check: ", chk)
   d += 2
}

def long_lived = dict_get(trees, max_depth, 0)
print(
   "Long-lived tree of depth ",
   max_depth,
   " check: ",
   check(long_lived)
)

def end = ticks()
print("Time: ", (end - start) / 1000000, " ms")

