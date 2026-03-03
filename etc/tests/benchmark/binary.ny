use std.core *
use std.core.error *
use std.core.reflect *
use std.core.dict *
use std.text.io *
use std.text.io *
use std.text *
use std.os *
use std.os.time *

;; Binary Trees (Benchmark)

fn _bench_scale_percent(){
   def raw = env("NYTRIX_BENCH_SCALE")
   if(is_str(raw) && str_len(raw) > 0){
      def v = atoi(raw)
      if(v > 0){ return v }
   }
   100
}

fn _bench_scale(val, minv){
   mut out = (val * _bench_scale_percent()) / 100
   if(out < minv){ out = minv }
   out
}

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
mut max_depth = _bench_scale(10, 6)
if(max_depth < min_depth){ max_depth = min_depth }

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
