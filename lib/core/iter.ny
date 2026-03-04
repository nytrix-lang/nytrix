;; Keywords: core iter functional
;; Iterable Collection and Functional Programming Utilities for Nytrix

module std.core.iter (
   range, range2, enumerate, map_list, filter_list, repeat, take, zip2,
   any, all, fold, find_if, find_index_if, chain, flatten, filter_map,
   zip_with, cycle, partition
)
use std.core *

fn any(xs, pred){
   "Returns **true** if `pred(v)` is truthy for **at least one** element in `xs`."
   def n = len(xs)
   mut i = 0
   while(i < n){
      if(pred(get(xs, i))){ return true }
      i += 1
   }
   false
}

fn all(xs, pred){
   "Returns **true** if `pred(v)` is truthy for **all** elements in `xs`."
   def n = len(xs)
   mut i = 0
   while(i < n){
      if(!pred(get(xs, i))){ return false }
      i += 1
   }
   true
}

fn fold(xs, init, fn2){
   "Reduces `xs` to a single value using `fn2(acc, v)`, starting with `init`."
   mut acc = init
   def n = len(xs)
   mut i = 0
   while(i < n){
      acc = fn2(acc, get(xs, i))
      i += 1
   }
   acc
}

fn find_if(xs, pred, default=0){
   "Returns the first element `v` in `xs` where `pred(v)` is truthy, or `default`."
   def n = len(xs)
   mut i = 0
   while(i < n){
      def v = get(xs, i)
      if(pred(v)){ return v }
      i += 1
   }
   default
}

fn find_index_if(xs, pred){
   "Returns the index of the first element in `xs` where `pred(v)` is truthy, or -1."
   def n = len(xs)
   mut i = 0
   while(i < n){
      if(pred(get(xs, i))){ return i }
      i += 1
   }
   -1
}

fn chain(xs, ys){
   "Concatenates two sequences into a new list."
   def n = len(xs)
   def m = len(ys)
   mut out = list(n + m)
   mut i = 0
   while(i < n){
      _list_set(out, i, get(xs, i))
      i += 1
   }
   mut j = 0
   while(j < m){
      _list_set(out, n + j, get(ys, j))
      j += 1
   }
   _list_finish(out, n + m)
}

fn flatten(xss){
   "Flattens a list of lists into a single list."
   mut total = 0
   def n = len(xss)
   mut i = 0
   while(i < n){
      def inner = get(xss, i)
      if(is_list(inner)){ total += len(inner) }
      else { total += 1 }
      i += 1
   }
   mut out = list(total)
   mut pos = 0
   i = 0
   while(i < n){
      def inner = get(xss, i)
      if(is_list(inner)){
         def m = len(inner)
         mut j = 0
         while(j < m){
         _list_set(out, pos, get(inner, j))
         pos += 1
         j += 1
         }
      } else {
         _list_set(out, pos, inner)
         pos += 1
      }
      i += 1
   }
   _list_finish(out, total)
}

fn filter_map(xs, fn1){
   "Maps `fn1(v)` over `xs` and keeps only the non-**none** results."
   def n = len(xs)
   mut out = list(n)
   mut i = 0
   mut pos = 0
   while(i < n){
      def res = fn1(get(xs, i))
      if(res){
         _list_set(out, pos, res)
         pos += 1
      }
      i += 1
   }
   _list_finish(out, pos)
}

fn zip_with(a, b, fn2){
   "Pairs items from `a` and `b` and applies `fn2(ai, bi)`. Truncates to shorter input."
   mut n = len(a)
   def m = len(b)
   if(m < n){ n = m }
   mut out = list(n)
   mut i = 0
   while(i < n){
      _list_set(out, i, fn2(get(a, i), get(b, i)))
      i += 1
   }
   _list_finish(out, n)
}

fn cycle(xs, count){
   "Repeats sequence `xs` infinitely (or `count` times)."
   if(count <= 0 || len(xs) == 0){ return list(0) }
   def n = len(xs)
   mut out = list(n * count)
   mut i = 0
   while(i < count){
      mut j = 0
      while(j < n){
         _list_set(out, i * n + j, get(xs, j))
         j += 1
      }
      i += 1
   }
   _list_finish(out, n * count)
}

fn partition(xs, pred){
   "Separates `xs` into two lists: `[ [truthy...], [falsy...] ]`."
   def n = len(xs)
   mut t = list(n)
   mut f = list(n)
   mut ti = 0
   mut fi = 0
   mut i = 0
   while(i < n){
      def v = get(xs, i)
      if(pred(v)){
         _list_set(t, ti, v)
         ti += 1
      } else {
         _list_set(f, fi, v)
         fi += 1
      }
      i += 1
   }
   [ _list_finish(t, ti), _list_finish(f, fi) ]
}

fn _range_count(start, stop, step){
   "Internal: calculates the number of elements in a range from `start` to `stop` with `step`."
   if(step == 0){ return 0 }
   if(step > 0){
      if(start >= stop){ return 0 }
      return ((stop - start - 1) / step) + 1
   }
   if(start <= stop){ return 0 }
   return ((start - stop - 1) / (0 - step)) + 1
}

fn _list_set(out, idx, value){
   "Internal: sets the element at `idx` in list `out` to `value` (bypassing safety checks)."
   store64(out, value, 16 + idx * 8)
}

fn _list_finish(out, len){
   "Internal: sets the final length of the provided list `out` to `len` and returns the list."
   store64(out, len, 0)
   out
}

fn range2(start, stop, step=1){
   "Returns a lazy Range object `[start, start+step, ...]` up to `stop` (exclusive)."
   mut st = step
   if(st == 0){ st = 1 }
   mut obj = malloc(24)
   store64(obj, 106, -8) ; Tag Range
   store64(obj, start, 0)
   store64(obj, stop, 8)
   store64(obj, st, 16)
   obj
}

fn range(a, b=0, step=1){
   "Compatibility range: `range(stop)` and `range(start, stop, step)`."
   if(b == 0 && step == 1){
      return range2(0, a, 1)
   }
   range2(a, b, step)
}

fn enumerate(xs, start=0){
   "Returns `[[index, value], ...]` over sequence `xs`."
   def n = len(xs)
   mut out = list(n)
   mut i = 0
   while(i < n){
      _list_set(out, i, [start + i, get(xs, i)])
      i += 1
   }
   _list_finish(out, n)
}

fn map_list(xs, fn1){
   "Applies `fn1(v)` to each element and returns a new list."
   def n = len(xs)
   mut out = list(n)
   mut i = 0
   while(i < n){
      _list_set(out, i, fn1(get(xs, i)))
      i += 1
   }
   _list_finish(out, n)
}

fn filter_list(xs, pred){
   "Returns elements `v` where `pred(v)` is truthy."
   def n = len(xs)
   mut out = list(n)
   mut i = 0
   mut idx = 0
   while(i < n){
      def v = get(xs, i)
      if(pred(v)){
         _list_set(out, idx, v)
         idx += 1
      }
      i += 1
   }
   _list_finish(out, idx)
}

fn repeat(value, count){
   "Returns a list with `count` copies of `value`."
   if(count <= 0){ return list(0) }
   mut out = list(count)
   mut i = 0
   while(i < count){
      _list_set(out, i, value)
      i += 1
   }
   _list_finish(out, count)
}

fn take(xs, count){
   "Returns the first `count` elements from sequence `xs`."
   if(count <= 0){ return list(0) }
   def n = len(xs)
   mut lim = count
   if(lim > n){ lim = n }
   mut out = list(lim)
   mut i = 0
   while(i < lim){
      _list_set(out, i, get(xs, i))
      i += 1
   }
   _list_finish(out, lim)
}

fn zip2(a, b){
   "Returns paired items `[[a0,b0], [a1,b1], ...]` up to the shorter input."
   mut n = len(a)
   def m = len(b)
   if(m < n){ n = m }
   if(n <= 0){ return list(0) }
   mut out = list(n)
   mut i = 0
   while(i < n){
      _list_set(out, i, [get(a, i), get(b, i)])
      i += 1
   }
   _list_finish(out, n)
}

if(comptime{__main()}){
   use std.core.iter as it
   use std.core.error *

   print("Testing std.core.iter...")

   assert((it.range(5) == [0, 1, 2, 3, 4]), "range(stop)")
   assert((it.range(2, 6) == [2, 3, 4, 5]), "range(start, stop)")
   assert((it.range(6, 2, -2) == [6, 4]), "range(start, stop, step)")

   assert((it.range2(1, 8, 3) == [1, 4, 7]), "range2")
   assert((it.enumerate(["a", "b"], 10) == [[10, "a"], [11, "b"]]), "enumerate")

   def sq = it.map_list([1, 2, 3], fn(v){
       "Auto-generated docstring: anonymous function."
       v * v
   })
   assert((sq == [1, 4, 9]), "map_list")

   def even = it.filter_list([1, 2, 3, 4, 5, 6], fn(v){
       "Auto-generated docstring: anonymous function."
       (v % 2) == 0
   })
   assert((even == [2, 4, 6]), "filter_list")

   assert((it.repeat("x", 3) == ["x", "x", "x"]), "repeat")
   assert((it.take([9, 8, 7], 2) == [9, 8]), "take")
   assert((it.zip2(["a", "b", "c"], [1, 2]) == [["a", 1], ["b", 2]]), "zip2")

   assert(it.any([1, 2, 3], fn(v){ v > 2 }), "any true")
   assert(!it.any([1, 2, 3], fn(v){ v > 5 }), "any false")
   assert(it.all([4, 5, 6], fn(v){ v > 3 }), "all true")
   assert(!it.all([4, 5, 6], fn(v){ v > 5 }), "all false")
   assert(it.fold([1, 2, 3, 4], 0, fn(a, v){ a + v }) == 10, "fold sum")
          assert(it.find_if([10, 20, 30], fn(v){ v > 15 }) == 20, "find")
          assert(it.find_if([10, 20, 30], fn(v){ v > 50 }, -1) == -1, "find missing")
          assert(it.find_index_if([10, 20, 30], fn(v){ v > 15 }) == 1, "find_index")
      assert(it.chain([1, 2], [3, 4]) == [1, 2, 3, 4], "chain")
   assert(it.flatten([[1, 2], 3, [4, 5]]) == [1, 2, 3, 4, 5], "flatten")
   assert(it.filter_map([1, 2, 3, 4], fn(v){
      if((v % 2) == 0){ return v * 10 }
      0
   }) == [20, 40], "filter_map")

   assert(it.zip_with([1, 2], [10, 20], fn(a, b){ a + b }) == [11, 22], "zip_with")
   assert(it.cycle([1, 2], 3) == [1, 2, 1, 2, 1, 2], "cycle")

   def pt = it.partition([1, 2, 3, 4, 5], fn(v){ v > 3 })
   assert(get(pt, 0) == [4, 5], "partition true")
   assert(get(pt, 1) == [1, 2, 3], "partition false")

   print("✓ std.core.iter tests passed")
}
