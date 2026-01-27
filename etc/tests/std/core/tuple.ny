use std.core.tuple *
use std.core.reflect *
use std.core *

;; Core Tuple (Test)
;; Tests tuple creation, type properties, and behavior.

print("Testing tuple creation...")
def t = tuple([1, 2, 3])
assert(is_tuple(t), "is_tuple returns true")
assert(list_len(t) == 3, "tuple length is 3")
assert(get(t, 0) == 1, "tuple get(0)")
assert(get(t, 1) == 2, "tuple get(1)")
assert(get(t, 2) == 3, "tuple get(2)")

print("Testing tuple immutability convention...")
; Nytrix doesn't strictly enforce immutability at runtime,
; but we expect it not to be a list according to is_list
def t2 = tuple([10, 20])
assert(!is_list(t2), "tuple is not a list")

print("Testing tuple type...")
def t3 = tuple([1, 2])
assert(eq(type(t3), "tuple"), "type(t) returns 'tuple'")

print("Testing tuple(none)...")
def t4 = tuple(0)
assert(is_tuple(t4), "tuple(none) returns empty tuple")
assert(list_len(t4) == 0, "empty tuple length is 0")

print("✓ std.core.tuple tests passed")
