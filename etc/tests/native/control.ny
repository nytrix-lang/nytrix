use std.core
use std.core.error
use std.core.reflect
use std.core.io

; If/else
fn test_if(i64 x) i64 {
  if x > 0 { 1 } else { -1 }
}
assert_eq(test_if(5), 1)
assert_eq(test_if(0), -1)
assert_eq(test_if(-3), -1)

; While loop
fn test_while(i64 n) i64 {
  mut i = 0
  mut sum = 0
  while i < n {
    sum = sum + i
    i = i + 1
  }
  sum
}
assert_eq(test_while(0), 0)
assert_eq(test_while(5), 10)
assert_eq(test_while(10), 45)

; Nested if
fn test_nested_if(i64 a, i64 b) i64 {
  if a > 0 {
    if b > 0 { a + b } else { a - b }
  } else {
    if b > 0 { b - a } else { a * b }
  }
}
assert_eq(test_nested_if(3, 4), 7)
assert_eq(test_nested_if(3, -1), 4)
assert_eq(test_nested_if(-2, 5), 7)
assert_eq(test_nested_if(-2, -3), 6)

; Short-circuit and/or
fn test_short_circuit_and(i64 x) bool {
  x > 0 && x < 10
}
assert_eq(test_short_circuit_and(5), true)
assert_eq(test_short_circuit_and(-1), false)
assert_eq(test_short_circuit_and(20), false)

fn test_short_circuit_or(i64 x) bool {
  x < 0 || x > 10
}
assert_eq(test_short_circuit_or(-5), true)
assert_eq(test_short_circuit_or(5), false)
assert_eq(test_short_circuit_or(15), true)

print("✓ native control tests passed")
