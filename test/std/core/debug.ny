use std.io
use std.core.debug
use std.util.inspect
use std.core.test
use std.core

print("Testing Debug & Inspect...")

debug_print("test_val", 123)
inspect(123)
inspect("hello")
inspect([1, 2])

print("✓ std.core.debug passed")
