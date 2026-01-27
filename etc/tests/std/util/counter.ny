use std.util.counter *
use std.core.list *
use std.core.error *

;; std.util.counter (Test)
;; Tests counter creation, updates, and most_common.

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
