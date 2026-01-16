use std.io.glob
use std.core.test
use std.collections
use std.strings.str

print("Testing Glob...")
assert(glob_match("*.ny", "foo.ny"), "glob match *.ny")
assert(glob_match("*.ny", "foo.c") == false, "glob fail")
assert(glob_match("f?o", "foo"), "glob ? match")
assert(glob_match("f?o", "bar") == false, "glob ? fail")

def xs = list()
xs = append(xs, "a.c")
xs = append(xs, "b.h")
xs = append(xs, "c.c")

def res = glob_filter("*.c", xs)
assert(list_len(res) == 2, "glob filter len")
assert(eq(get(res, 0), "a.c"), "filter 0")
assert(eq(get(res, 1), "c.c"), "filter 1")

print("✓ std.io.glob passed")
