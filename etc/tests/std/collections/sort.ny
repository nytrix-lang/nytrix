use std.io
use std.collections.sort
use std.core.error
use std.core

;; collections.sort (Test)
;; Tests in-place sort and sorted (copying sort) functions.

print("Testing sort...")
def lst = list()
lst = append(lst, 5)
lst = append(lst, 2)
lst = append(lst, 9)
lst = append(lst, 1)
lst = append(lst, 5)
sort(lst)
assert(get(lst, 0) == 1, "sort 0")
assert(get(lst, 1) == 2, "sort 1")
assert(get(lst, 2) == 5, "sort 2")
assert(get(lst, 3) == 5, "sort 3")
assert(get(lst, 4) == 9, "sort 4")

print("Testing sorted...")
def lst2 = list()
lst2 = append(lst2, 3)
lst2 = append(lst2, 1)
lst2 = append(lst2, 2)
def s = sorted(lst2)
assert(get(s, 0) == 1, "sorted 0")
assert(get(s, 1) == 2, "sorted 1")
assert(get(s, 2) == 3, "sorted 2")
assert(get(lst2, 0) == 3, "original unsorted")

print("✓ std.collections.sort tests passed")
