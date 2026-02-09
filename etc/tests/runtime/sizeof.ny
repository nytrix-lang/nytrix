use std.core *

layout Point {
   x: i32,
   y: i32
}

struct Pair {
   a: i64,
   b: u8
}

assert(sizeof(int) == 8, "sizeof int")
assert(sizeof(i32) == 4, "sizeof i32")
assert(sizeof(u16) == 2, "sizeof u16")
assert(sizeof(f32) == 4, "sizeof f32")
assert(sizeof(f64) == 8, "sizeof f64")
assert(sizeof(f128) == 16, "sizeof f128")
assert(sizeof(Point) == 8, "sizeof layout Point")
assert(sizeof(Pair) == 16, "sizeof struct Pair")
assert(sizeof(*Point) == 8, "sizeof pointer")

print("✓ all runtime sizeof tests passed")
