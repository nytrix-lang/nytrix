use std.core
use std.core.error
use std.core.reflect
use std.core.io

; Simple mut variable
fn test_mut() i64 {
  mut x = 10
  x = x + 5
  x
}
assert_eq(test_mut(), 15)

; Multiple muts
fn test_multi_mut() i64 {
  mut a = 1
  mut b = 2
  a = a + b
  b = b * 3
  a + b
}
assert_eq(test_multi_mut(), 9)

; Mut in loop
fn test_mut_loop() i64 {
  mut x = 1
  mut i = 0
  while i < 5 {
    x = x * 2
    i = i + 1
  }
  x
}
assert_eq(test_mut_loop(), 32)

; Mut accum
fn sum_to(i64 n) i64 {
  mut s = 0
  mut i = 1
  while i <= n {
    s = s + i
    i = i + 1
  }
  s
}
assert_eq(sum_to(10), 55)
assert_eq(sum_to(100), 5050)

print("✓ native variable tests passed")
