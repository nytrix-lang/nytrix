use std.math.timefmt *
use std.core.error *

;; std.math.timefmt (Test)
;; Tests timestamp formatting.

print("Testing timefmt...")

def ts = 1673712000
def formatted = format_time(ts)
assert(eq(formatted, "2023-01-14 16:00:00"), "format_time")

print("✓ std.math.timefmt tests passed")
