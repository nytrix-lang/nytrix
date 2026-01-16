use std.io
use std.math.stat
use std.core.test
use std.core
use std.strings.str

print("Testing Math Stat...")

def xs = [1, 2, 3, 4, 5]
assert(mean(xs) == 3, "mean")
assert(median(xs) == 3, "median odd")

def ys = [1, 2, 3, 4]
; mean = 2.5 -> int div -> 2?
; 10/4 = 2.
assert(mean(ys) == 2, "mean int")
; median: (2+3)/2 = 2.
assert(median(ys) == 2, "median even")

print("✓ std.math.stat passed")
