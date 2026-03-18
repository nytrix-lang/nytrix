use std.core
use std.os.time
use benchmark.helpers

;; Binary Trees (Benchmark)
fn make_tree(int: depth): any {
   if(depth == 0){ return 0 }
   mut d = depth - 1
   return [make_tree(d), make_tree(d)]
}

fn check(any: t): int {
   if(t == 0){ return 1 }
   return 1 + check(t.get(0)) + check(t.get(1))
}

def min_depth = 4
mut max_depth = _bench_scale(10, 6)

if(max_depth < min_depth){ max_depth = min_depth }
print("Benchmarking Binary Trees(fast mode, max depth ", max_depth, ")")
def start = ticks()
mut trees = dict()
mut d = min_depth
while(d <= max_depth){
   trees = trees.set(d, make_tree(d))
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
   def t = trees.get(d, 0)
   def chk = check(t) * iterations
   print(iterations, " trees of depth ", d, " check: ", chk)
   d += 2
}

def long_lived = trees.get(max_depth, 0)
print(
   "Long-lived tree of depth ",
   max_depth,
   " check: ",
   check(long_lived)
)

def end = ticks()
print("Time: ", (end - start) / 1000000, " ms")
