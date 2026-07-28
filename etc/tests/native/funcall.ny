use std.core
use std.core.error
use std.core.reflect
use std.core.io

; Simple function call
fn double(i64 x) i64 { x * 2 }
assert_eq(double(21), 42)

; Multiple args
fn add3(i64 a, i64 b, i64 c) i64 { a + b + c }
assert_eq(add3(1, 2, 3), 6)

; Chained function calls
fn square(i64 x) i64 { x * x }
fn add_then_square(i64 a, i64 b) i64 { square(a + b) }
assert_eq(add_then_square(3, 4), 49)

; Recursion
fn factorial(i64 n) i64 {
  if n <= 1 { 1 } else { n * factorial(n - 1) }
}
assert_eq(factorial(0), 1)
assert_eq(factorial(1), 1)
assert_eq(factorial(5), 120)
assert_eq(factorial(10), 3628800)

; Tail recursion
fn tail_sum(i64 n, i64 acc) i64 {
  if n <= 0 { acc } else { tail_sum(n - 1, acc + n) }
}
assert_eq(tail_sum(5, 0), 15)
assert_eq(tail_sum(100, 0), 5050)

; Deep call chain
fn inc(i64 x) i64 { x + 1 }
fn add2(i64 x) i64 { inc(inc(x)) }
fn add4(i64 x) i64 { add2(add2(x)) }
fn add8(i64 x) i64 { add4(add4(x)) }
assert_eq(add8(0), 8)
assert_eq(add8(10), 18)

print("✓ native function call tests passed")
