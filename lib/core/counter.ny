;; Keywords: counter frequency multiset
;; Frequency counters, tallies, and count-based collection summaries.
module std.core.counter(counter, counter_add, most_common)
use std.core
use std.core.reflect

@returns_owned
fn counter(seq: xs): dict {
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
fn counter_add(dict: d, any: key, int: n): dict {
   "Adds `n` to the count of `key` in counter dictionary `d`."
   d[key] = d.get(key, 0) + n
   d
}

@returns_owned
fn most_common(dict: d): list {
   "Returns a list of `[item, count]` pairs from counter `d`, sorted by count in descending order."
   def its = items(d)
   def n = its.len
   ; selection sort by count desc
   mut i = 0
   while(i < n){
      mut max_idx = i
      mut max_count = its.get(i).get(1)
      mut j = i + 1
      while(j < n){
         def count_j = its.get(j).get(1)
         if(count_j > max_count){
            max_idx = j
            max_count = count_j
         }
         j += 1
      }
      if(max_idx != i){
         def tmp = its.get(i)
         its[i] = its.get(max_idx)
         its[max_idx] = tmp
      }
      i += 1
   }
   its
}
