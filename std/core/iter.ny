;; Keywords: core iter
;; Iter helpers.

module std.core.iter (
   range, range2, enumerate, map_list, filter_list, repeat, take, zip2
)
use std.core *

fn _range_count(start, stop, step){
   "Internal helper."
   if(step == 0){ return 0 }
   if(step > 0){
      if(start >= stop){ return 0 }
      return ((stop - start - 1) / step) + 1
   }
   if(start <= stop){ return 0 }
   return ((start - stop - 1) / (0 - step)) + 1
}

fn _list_set(out, idx, value){
   "Internal helper."
   store64(out, value, 16 + idx * 8)
}

fn _list_finish(out, len){
   "Internal helper."
   store64(out, len, 0)
   out
}

fn range2(start, stop, step=1){
   "Returns `[start, start+step, ...]` up to `stop` (exclusive)."
   mut st = step
   if(st == 0){ st = 1 }
   def cnt = _range_count(start, stop, st)
   mut out = list(cnt)
   mut i = start
   mut idx = 0
   if(st > 0){
      while(i < stop){
         _list_set(out, idx, i)
         idx += 1
         i = i + st
      }
   } else {
      while(i > stop){
         _list_set(out, idx, i)
         idx += 1
         i = i + st
      }
   }
   _list_finish(out, idx)
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

    def sq = it.map_list([1, 2, 3], fn(v){ v * v })
    assert((sq == [1, 4, 9]), "map_list")

    def even = it.filter_list([1, 2, 3, 4, 5, 6], fn(v){ (v % 2) == 0 })
    assert((even == [2, 4, 6]), "filter_list")

    assert((it.repeat("x", 3) == ["x", "x", "x"]), "repeat")
    assert((it.take([9, 8, 7], 2) == [9, 8]), "take")
    assert((it.zip2(["a", "b", "c"], [1, 2]) == [["a", 1], ["b", 2]]), "zip2")

    print("✓ std.core.iter tests passed")
}
