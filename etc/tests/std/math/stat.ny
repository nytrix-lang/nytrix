use std.math.stat *
use std.core.error *

;; std.math.stat (Test)
;; Tests mean and median for odd and even datasets.

print("Testing Math Stat...")

def xs = [1, 2, 3, 4, 5]
assert(mean(xs) == 3, "mean odd")
assert(sum(xs) == 15, "sum odd")
assert(median(xs) == 3, "median odd")

def ys = [1, 2, 3, 4]
assert(mean(ys) == 2, "mean even int")
assert(median(ys) == 2, "median even")

print("✓ std.math.stat tests passed")
