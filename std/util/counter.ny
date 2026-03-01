;; Keywords: util counter
;; Util Counter module.

module std.util.counter (
   counter, counter_add, most_common
)
use std.core *
use std.core.reflect *

fn counter(xs){
   "Creates a frequency counter dictionary from the elements of list or string `xs`."
   mut d = dict(16)
   mut i = 0  def n = len(xs)
   while(i < n){
      def v = get(xs, i)
      def c = dict_get(d, v, 0)
      d = dict_set(d, v, c + 1)
      i += 1
   }
   return d
}

fn counter_add(d, key, n){
   "Adds `n` to the count of `key` in counter dictionary `d`."
   def c = dict_get(d, key, 0)
   return dict_set(d, key, c + n)
}

fn most_common(d){
   "Returns a list of `[item, count]` pairs from counter `d`, sorted by count in descending order."
   def its = items(d)
   def n = len(its)
   ; selection sort by count desc
   mut i = 0
   while(i < n){
      mut max_idx = i
      mut max_count = get(get(its, i), 1)
      mut j = i + 1
      while(j < n){
         def count_j = get(get(its, j), 1)
         if(count_j > max_count){
            max_idx = j
            max_count = count_j
         }
         j += 1
      }
      if(max_idx != i){
         def tmp = get(its, i)
         store_item(its, i, get(its, max_idx))
         store_item(its, max_idx, tmp)
      }
      i += 1
   }
   return its
}

if(comptime{__main()}){
    use std.util.counter *
    use std.core *
    use std.core.dict *
    use std.core.list *
    use std.core.error *

    print("Testing Util Counter...")

    def xs = ["a", "b", "a", "c", "b", "a"]
    mut c = counter(xs)

    assert(dict_get(c, "a", 0) == 3, "count a")
    assert(dict_get(c, "b", 0) == 2, "count b")
    assert(dict_get(c, "c", 0) == 1, "count c")
    assert(dict_get(c, "d", 0) == 0, "count d")

    c = counter_add(c, "d", 5)
    assert(dict_get(c, "d", 0) == 5, "count add")

    def common = most_common(c)
    print(f"Common len: {len(common)}")
    for item in common {
       print(f"  Item: {item} type={type(item)}")
       if(is_list(item)){
          print(f"    [0]: {get(item, 0)} type={type(get(item, 0))}")
          print(f"    [1]: {get(item, 1)} type={type(get(item, 1))}")
       }
    }

    def pair0 = get(common, 0)
    assert(get(pair0, 0) == "d", "most common 0 key")
    assert(get(pair0, 1) == 5, "most common 0 val")

    def pair1 = get(common, 1)
    assert(get(pair1, 0) == "a", "most common 1 key")

    print("✓ std.util.counter tests passed")
}
