use std.core *
use std.core.error *

print("Testing std.core.match...")

def a = 4
def r1 = match a {
   4 if a > 1 -> "ok"
   4 -> "bad"
   _ -> "no"
}
assert_eq(r1, "ok", "literal match guard")

def x = ok(42)
def r2 = match x {
   ok(v) if v > 40 -> v
   ok(v) -> v + 1
   err(_) -> -1
   _ -> -2
}
assert_eq(r2, 42, "ok(v) guard")

def y = ok(2)
def r3 = match y {
   ok(v) if v > 10 -> 99
   ok(v) -> v
   _ -> 0
}
assert_eq(r3, 2, "guard fallthrough")

print("✓ std.core.match tests passed")
