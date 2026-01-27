use std.io
use std.io.glob
use std.collections
use std.strings.str
use std.core.error

;; std.io.glob (Test)
;; Tests glob matching and filtering.

print("Testing Glob...")
assert(glob_match("*.ny", "foo.ny"), "glob match *.ny")
assert(glob_match("*.ny", "foo.c") == false, "glob fail")
assert(glob_match("f?o", "foo"), "? match")
assert(glob_match("f?o", "bar") == false, "? fail")

def xs = list()
xs = append(xs, "a.c")
xs = append(xs, "b.h")
xs = append(xs, "c.c")

def res = glob_filter("*.c", xs)
assert(list_len(res) == 2, "filter len")
assert(eq(get(res, 0), "a.c"), "filter 0")
assert(eq(get(res, 1), "c.c"), "filter 1")

print("✓ std.io.glob tests passed")
