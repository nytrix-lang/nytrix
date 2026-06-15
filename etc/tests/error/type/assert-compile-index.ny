;; expect: static index proof failed
use std.core

def xs = [1, 2, 3]
def int i = 4
assert_compile_index(xs, i, "static index proof failed")
print(xs)
