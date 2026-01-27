use std.core *
use std.str.str *
use std.str *

;; std.str.str (Test)

def s = "abcdef"
assert(_str_eq(str_slice(s, 0, 3), "abc"), "slice start")
assert(_str_eq(str_slice(s, 1, 5, 2), "bd"), "slice step")
assert(_str_eq(str_slice(s, -3, -1), "de"), "slice negative")
assert(_str_eq(str_slice(s, 0, 0), ""), "slice empty")

print("✓ std.str.str tests passed")
