;; Keywords: counter frequency multiset core
;; Frequency counters, tallies, and count-based collection summaries.
;; References:
;; - std.core
module std.core.counter(counter, counter_add, most_common)
use std.core
use std.core.reflect

@returns_owned
fn counter(seq xs) dict {
   "Creates a frequency counter dictionary from the elements of list or string `xs`."
   mut d, i = dict(16), 0
   def n = xs.len
   while(i < n){
      def v = xs.get(i)
      d[v] = d.get(v, 0) + 1
      i += 1
   }
   d
}

@returns_owned
@consumes(d)
fn counter_add(dict d, any key, int n) dict {
   "Adds `n` to the count of `key` in counter dictionary `d`."
   d[key] = d.get(key, 0) + n
   d
}

@returns_owned
fn _counter_merge_common(list left, list right) list {
   def ln, rn = left.len, right.len
   mut out = list(ln + rn)
   mut i, j = 0, 0
   while(i < ln && j < rn){
      def li = left.get(i)
      def rj = right.get(j)
      if(li.get(1, 0) >= rj.get(1, 0)){
         out = out.append(li)
         i += 1
      } else {
         out = out.append(rj)
         j += 1
      }
   }
   while(i < ln){
      out = out.append(left.get(i))
      i += 1
   }
   while(j < rn){
      out = out.append(right.get(j))
      j += 1
   }
   out
}

@returns_owned
fn _counter_sort_common(list xs) list {
   def n = xs.len
   if(n <= 1){ return xs }
   def mid = n / 2
   def left = slice(xs, 0, mid, 1)
   def right = slice(xs, mid, n, 1)
   _counter_merge_common(_counter_sort_common(left), _counter_sort_common(right))
}

@returns_owned
fn most_common(dict d) list {
   "Returns a list of `[item, count]` pairs from counter `d`, sorted by count in descending order."
   _counter_sort_common(items(d))
}

#main {
   def xs = ["a", "b", "a", "c", "b", "a"]
   mut c = counter(xs)
   assert(c.get("a", 0) == 3, "counter count a")
   assert(c.get("b", 0) == 2, "counter count b")
   def base_common = most_common(c)
   assert(base_common.get(0).get(0) == "a" && base_common.get(0).get(1) == 3, "counter most_common base")
   c = counter_add(c, "d", 5)
   def common = most_common(c)
   assert(common.get(0).get(0) == "d", "counter most_common first key")
   assert(common.get(0).get(1) == 5, "counter most_common first value")
   assert(common.get(1).get(0) == "a", "counter most_common second key")
   print("✓ std.core.counter self-test passed")
}
