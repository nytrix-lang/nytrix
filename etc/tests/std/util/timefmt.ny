use std.util.timefmt *
use std.core *
use std.math.timefmt *

;; std.util.timefmt (Test)
;; Tests timestamp formatting.

print("Testing timefmt...")

def ts = 1673712000
def formatted = format_time(ts)
assert(eq(formatted, "2023-01-14 16:00:00"), "format_time")

print("✓ std.util.timefmt tests passed")
