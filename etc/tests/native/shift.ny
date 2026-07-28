use std.core
use std.core.error
use std.core.reflect
use std.core.io

; Left shift
assert_eq(1 << 0, 1)
assert_eq(1 << 1, 2)
assert_eq(1 << 10, 1024)
assert_eq(7 << 3, 56)

; Right shift is arithmetic for signed i64 values.
assert_eq(1024 >> 10, 1)
assert_eq(56 >> 3, 7)
assert_eq(16 >> 2, 4)
assert_eq(0xF0 >> 4, 0x0F)

; Compound with shifts
fn scale(i64 x) i64 { (x << 2) + (x >> 1) }
assert_eq(scale(10), 45)

; Shift and mask
fn extract_byte(i64 x, i64 n) i64 { (x >> (n * 8)) & 0xFF }
assert_eq(extract_byte(0xAABBCCDD, 0), 0xDD)
assert_eq(extract_byte(0xAABBCCDD, 1), 0xCC)
assert_eq(extract_byte(0xAABBCCDD, 2), 0xBB)
assert_eq(extract_byte(0xAABBCCDD, 3), 0xAA)

print("✓ native shift tests passed")
