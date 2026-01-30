use std.core.debug *
use std.util.inspect *
use std.core.test *
use std.core *

;; Core Debug (Test)
;; Tests debugging utilities including debug_print and object inspection.

print("Testing Debug & Inspect...")

debug_print("test_val", 123)
inspect(123)
inspect("hello")
inspect([1, 2])

print("✓ std.core.debug tests passed")
