use std.io
use std.util.inspect
use std.core.error

;; std.util.inspect (Test)
;; Tests inspect on basic types.

print("Testing inspect...")

inspect(123)
inspect("hello")
inspect([1, 2, 3])
inspect({ "a": 1 })
inspect(0)

print("✓ std.util.inspect tests passed")
