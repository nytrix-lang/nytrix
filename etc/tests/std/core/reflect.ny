use std.os.time *
use std.core.reflect *
use std.math.float *
use std.core *

;; Core Reflect (Test)
;; Tests introspection, type checking, and reflection capabilities.

; Type
assert(eq(type(42), "int"), "type of integer")
assert(eq(type("hello"), "str"), "type of string")
assert(eq(type([1, 2, 3]), "list"), "type of list")
assert(eq(type(dict(8)), "dict"), "type of dict")
assert(eq(type(set()), "set"), "type of set")
assert(eq(type(float(1)), "float"), "type of float")
assert(eq(type(true), "bool"), "type of bool")
assert(eq(type(0), "none"), "type of none")

; Len
assert(len([1, 2, 3]) == 3, "len of list")
assert(len("hello") == 5, "len of string")
assert(len([]) == 0, "len of empty list")
mut d = dict(8)
d = dict_set(d, "key", "value")
assert(len(d) == 1, "len of dict")
assert(len(float(1)) == 0, "len of float")

; Contains
def lst = [1, 2, 3, 4, 5]
assert(contains(lst, 3), "list contains element")
assert(!contains(lst, 10), "list doesn't contain element")
mut s = set()
s = set_add(s, "a")
s = set_add(s, "b")
assert(contains(s, "a"), "set contains element")
assert(!contains(s, "c"), "set doesn't contain element")
assert(contains("hello world", "world"), "string contains substring")
assert(!contains("hello", "xyz"), "string doesn't contain substring")

; Eq
assert(eq(42, 42), "int equality")
assert(!eq(42, 43), "int inequality")
assert(eq("hello", "hello"), "string equality")
assert(!eq("hello", "world"), "string inequality")
assert(eq([1, 2, 3], [1, 2, 3]), "list equality")
assert(!eq([1, 2, 3], [1, 2, 4]), "list inequality")
assert(!eq([1, 2], [1, 2, 3]), "list different lengths")
mut d1 = dict(8)
d1 = dict_set(d1, "a", 1)
d1 = dict_set(d1, "b", 2)
mut d2 = dict(8)
d2 = dict_set(d2, "a", 1)
d2 = dict_set(d2, "b", 2)
assert(eq(d1, d2), "dict equality")
assert(eq(float(1), float(1)), "float equality")

; Repr
assert(eq(repr(42), "42"), "repr of int")
assert(eq(repr(true), "true"), "repr of true")
assert(eq(repr(false), "false"), "repr of false")
assert(eq(repr(0), "none"), "repr of none")
assert(eq(repr("hello"), "\"hello\""), "repr of string")
assert(eq(repr([1,2,3]), "[1,2,3]"), "repr of list")

; Hash
mut h1 = hash(42)
mut h2 = hash(42)
assert(h1 == h2, "hash consistency for int")
h1 = hash("hello")
h2 = hash("hello")
assert(h1 == h2, "hash consistency for string")
def h3 = hash("world")
assert(h1 != h3, "different strings have different hashes")

print("✓ std.core.reflect tests passed")
