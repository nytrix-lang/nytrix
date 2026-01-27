use std.io
use std.collections
use std.core.error

;; std.collections.mod (Test)
;; Tests dict, set, list helpers, and sorted utilities.

print("Testing dict ops...")
def d = dict(8)
assert(is_dict(d), "dict creation")

dict_set(d, "name", "Nytrix")
dict_set(d, "version", "0.1")
dict_set(d, "year", 2026)

assert(eq(dict_get(d, "name", 0), "Nytrix"), "get string")
assert(dict_get(d, "year", 0) == 2026, "get int")
assert(contains(d, "name"), "has key")
assert(!contains(d, "missing"), "missing key")
assert(list_len(keys(d)) == 3, "keys count")
assert(list_len(values(d)) == 3, "values count")
print("Dict ops passed")

print("Testing set ops...")
def s = set()
assert(is_set(s), "set creation")

set_add(s, 1)
set_add(s, 2)
set_add(s, 3)
set_add(s, 2)

assert(set_contains(s, 1), "contains 1")
assert(set_contains(s, 2), "contains 2")
assert(!set_contains(s, 10), "missing element")

set_remove(s, 2)
assert(!set_contains(s, 2), "removed element")
print("Set ops passed")

print("Testing list helpers...")
def lst = [3, 1, 4, 1, 5, 9, 2, 6]
assert(list_contains(lst, 4), "contains element")
assert(!list_contains(lst, 10), "missing element")

def rev = list_reverse(lst)
assert(get(rev, 0) == 6, "rev first")
assert(get(rev, -1) == 3, "rev last")
print("List helpers passed")

print("Testing sorted...")
def sorted_lst = sorted(lst)
assert(get(sorted_lst, 0) == 1, "sorted first")
assert(get(sorted_lst, -1) == 9, "sorted last")

def i = 0
while(i < list_len(sorted_lst) - 1){
   assert(get(sorted_lst, i) <= get(sorted_lst, i + 1), "ascending")
   i = i + 1
}
print("Sorted passed")

print("✓ std.collections.mod tests passed")
