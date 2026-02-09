use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *

;; std.str (Test)
;; Basic string operations and parsing.

def s1 = "hello"
def s2 = "world"
print(f"DEBUG: len(s1) = {len(s1)}")
assert(len(s1) == 5, "len s1")
assert(len(s2) == 5, "len s2")

assert(eq(s1, "hello"), "eq")

def empty = ""
assert(len(empty) == 0, "empty len")

def concat = str_add(s1, s2)
assert(len(concat) == 10, "concat len")
def plus = s1 + s2
assert(eq(plus, concat), "plus concat")

def num_str = "42"
assert(len(num_str) == 2, "num str len")

assert(eq(upper("hello"), "HELLO"), "upper")
assert(eq(lower("WORLD"), "world"), "lower")

def search_str = "hello world"
assert(startswith(search_str, "hello"), "startswith")
assert(endswith(search_str, "world"), "endswith")
assert(str_contains(search_str, "world"), "contains")

def trimmed = strip("  hello  ")
assert(eq(trimmed, "hello"), "strip")

def parts = split("a,b,c,d", ",")
assert(len(parts) == 4, "split len")
assert(eq(get(parts, 0), "a"), "split 0")
assert(eq(get(parts, 3), "d"), "split 3")

assert(eq(join(["x","y","z"], "-"), "x-y-z"), "join")

assert(eq(str_replace("hello world", "world", "universe"), "hello universe"), "replace")

print("✓ std.str basic tests passed")
