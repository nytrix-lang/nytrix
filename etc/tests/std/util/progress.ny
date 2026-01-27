use std.io
use std.util.progress
use std.collections
use std.core.error

;; std.util.progress (Test)
;; Tests progress_map and manual progress.

print("Testing Util Progress...")

def xs = [1, 2, 3]
def res = progress_map(fn(x){ x * 2 }, xs, "Test Map")
assert(len(res) == 3, "progress_map len")
assert(get(res, 0) == 2, "progress_map val")

def p = progress(10, "Manual")
progress_update(p, 5)
progress_finish(p)

print("✓ std.util.progress tests passed")
