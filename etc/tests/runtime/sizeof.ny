use std.core *
use std.os *

layout Point {
   x: i32,
   y: i32
}

struct Pair {
   a: i64,
   b: u8
}

def int_sz = sizeof(int)
assert(int_sz == 4 || int_sz == 8, "sizeof int")
assert(sizeof(i32) == 4, "sizeof i32")
assert(sizeof(u16) == 2, "sizeof u16")
assert(sizeof(f32) == 4, "sizeof f32")
assert(sizeof(f64) == 8, "sizeof f64")
assert(sizeof(f128) == 16, "sizeof f128")
assert(sizeof(Point) == 8, "sizeof layout Point")
assert(sizeof(Pair) == 16, "sizeof struct Pair")
def ptr_sz = sizeof(*Point)
assert(ptr_sz == sizeof(str), "sizeof pointer")

print("✓ all runtime sizeof tests passed")
