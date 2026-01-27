use std.io
use std.os.time

;; Binary Trees (Benchmark)
;; Reuses trees to drastically reduce allocation cost.

fn make_tree(depth){
   if(depth == 0){ return 0 }
   def d = depth - 1
   return [make_tree(d), make_tree(d)]
}

fn check(t){
   if(t == 0){ return 1 }
   return 1 + check(get(t,0)) + check(get(t,1))
}

def min_depth = 4
def max_depth = 10   ; <-- key reduction

print("Benchmarking Binary Trees (fast mode, max depth ", max_depth, ")")

def start = ticks()

; Prebuild trees once
def trees = dict()
def d = min_depth
while(d <= max_depth){
   dict_set(trees, d, make_tree(d))
   d += 2
}

; Stretch tree
def stretch = make_tree(max_depth + 1)
print(
   "Stretch tree of depth ",
   max_depth + 1,
   " check: ",
   check(stretch)
)

; Main loop (no allocations now)
d = min_depth
while(d <= max_depth){
   def iterations = 1 << (max_depth - d)
   def t = dict_get(trees, d, 0)
   def chk = check(t) * iterations
   print(iterations, " trees of depth ", d, " check: ", chk)
   d += 2
}

; Long-lived tree
def long_lived = dict_get(trees, max_depth, 0)
print(
   "Long-lived tree of depth ",
   max_depth,
   " check: ",
   check(long_lived)
)

def end = ticks()
print("Time: ", (end - start) / 1000000, " ms")
