use std.io
use std.io.fmt
use std.core.error

;; std.io.fmt (Test)
;; Tests basic format string functionality.

print("Testing format...")
def s = format("Hello {}!", "World")
assert(eq(s, "Hello World!"), "format string")
s = format("{} + {} = {}", 1, 2, 3)
assert(eq(s, "1 + 2 = 3"), "format ints")
print("Format passed")

print("✓ std.io.fmt tests passed")
