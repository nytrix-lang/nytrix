use std.io
use std.collections.set
use std.collections.dict
use std.core.error

;; Set Collections (Test)
;; Tests set basic operations, union, intersection, difference, and stress cycles.

print("Testing set basic operations...")
def s = set()
s = set_add(s, 10)
s = set_add(s, 20)
assert(set_contains(s, 10), "Contains 10")
assert(set_contains(s, 20), "Contains 20")
assert(!set_contains(s, 30), "Does not contain 30")
s = set_remove(s, 10)
assert(!set_contains(s, 10), "Removed 10")
assert(set_contains(s, 20), "Still has 20")

print("Testing set union/intersection/difference...")
def s1 = set()
set_add(s1, 1)
set_add(s1, 2)
def s2 = set()
set_add(s2, 2)
set_add(s2, 3)
def u = set_union(s1, s2)
assert(set_contains(u, 1), "Union has 1")
assert(set_contains(u, 2), "Union has 2")
assert(set_contains(u, 3), "Union has 3")
def i = set_intersection(s1, s2)
assert(!set_contains(i, 1), "Intersection no 1")
assert(set_contains(i, 2), "Intersection has 2")
assert(!set_contains(i, 3), "Intersection no 3")
def d = set_difference(s1, s2)
assert(set_contains(d, 1), "Diff has 1")
assert(!set_contains(d, 2), "Diff no 2")
assert(!set_contains(d, 3), "Diff no 3")

print("Testing mixed types in set...")
def s3 = set()
set_add(s3, 100)
set_add(s3, "str")
assert(set_contains(s3, 100), "Contains int")
assert(set_contains(s3, "str"), "Contains str")
assert(!set_contains(s3, 101), "No int")
assert(!set_contains(s3, "other"), "No str")

print("Testing set stress cycle...")
def s4 = set()
def j = 0
while(j < 50){
   s4 = set_add(s4, j)
   j = j + 1
}
j = 0
while(j < 50){
   assert(set_contains(s4, j), "Contains value")
   j = j + 1
}
j = 0
while(j < 50){
   s4 = set_remove(s4, j)
   j = j + 1
}
j = 0
while(j < 50){
   assert(!set_contains(s4, j), "Removed value")
   j = j + 1
}

print("Testing set clear...")
def s5 = set()
set_add(s5, 1)
set_add(s5, 2)
set_clear(s5)
assert(!set_contains(s5, 1), "Cleared 1")
assert(!set_contains(s5, 2), "Cleared 2")
set_add(s5, 3)
assert(set_contains(s5, 3), "Re-used after clear")

print("Testing set method aliases...")
def s6 = set()
set_add(s6, 10)
assert(set_contains(s6, 10), "add")
set_remove(s6, 10)
assert(!set_contains(s6, 10), "remove")
set_add(s6, 20)
def s7 = set_copy(s6)
assert(set_contains(s7, 20), "alias copy")
set_clear(s6)
assert(!set_contains(s6, 20), "alias clear")

print("✓ std.collections.set tests passed")
