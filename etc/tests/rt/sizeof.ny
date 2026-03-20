use std.core
use std.os

layout TestPoint {
   x: i32,
   y: i32
}

struct TestPair {
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
assert(sizeof(TestPoint) == 8, "sizeof layout TestPoint")
assert(sizeof(TestPair) == 16, "sizeof struct TestPair")
def ptr_sz = sizeof(*TestPoint)
assert(ptr_sz == sizeof(str), "sizeof pointer")
print("✓ all runtime sizeof tests passed")
