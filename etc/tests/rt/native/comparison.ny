use std.core
use std.core.error
use std.core.reflect
use std.core.io

; All comparison ops
fn classify(i64 x) i64 {
  if x == 0 { 0 }
  elif x < 0 { -1 }
  else { 1 }
}
assert_eq(classify(0), 0)
assert_eq(classify(-5), -1)
assert_eq(classify(10), 1)

fn in_range(i64 x, i64 lo, i64 hi) bool { x >= lo && x <= hi }
assert_eq(in_range(5, 1, 10), true)
assert_eq(in_range(0, 1, 10), false)
assert_eq(in_range(11, 1, 10), false)
assert_eq(in_range(1, 1, 10), true)
assert_eq(in_range(10, 1, 10), true)

; Not-equal chains
fn clamp(i64 x, i64 lo, i64 hi) i64 {
  if x < lo { lo }
  elif x > hi { hi }
  else { x }
}
assert_eq(clamp(5, 1, 10), 5)
assert_eq(clamp(0, 1, 10), 1)
assert_eq(clamp(20, 1, 10), 10)

; Equality on bool returns
fn both_positive(i64 a, i64 b) bool { a > 0 && b > 0 }
assert_eq(both_positive(3, 5), true)
assert_eq(both_positive(-1, 5), false)
assert_eq(both_positive(3, 0), false)

print("✓ native comparison tests passed")
