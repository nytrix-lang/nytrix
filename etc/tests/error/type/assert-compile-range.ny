;; expect: compile-time range assertion failed
use std.core

def int x = 9
assert_compile_range(x, 0, 3)
print(x)
