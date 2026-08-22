use std.core
use std.core.error

; Regression: native f64 comparisons previously always returned true
; because the CMP result was only written to the home slot while the
; following STORE read the (stale) colored preg.
assert_eq(1.5 < 1.0, false)
assert_eq(1.0 < 1.5, true)
assert_eq(2.0 < 1.0, false)
assert_eq(1.5 > 1.0, true)
assert_eq(1.5 >= 1.5, true)
assert_eq(1.5 <= 1.5, true)
assert_eq(1.5 == 1.5, true)
assert_eq(1.5 != 1.5, false)

; Compare through locals (loads from home slots).
def f64 a = 1.5
def f64 b = 1.0
def f64 c = 2.5
assert_eq(a < b, false)
assert_eq(a > b, true)
assert_eq(a < c, true)
assert_eq(a == a, true)
assert_eq(a != b, true)

; Mixed int/float arithmetic: the i64 operand must be converted through
; the colored preg (cvtsi2sd), not a stale home read.
assert_eq(1.0 + 2.0, 3)
assert_eq(1 + 2.0, 3)
assert_eq(1.0 + 2, 3)
assert_eq(1 + 2.5, 3.5)
assert_eq(1.5 * 2, 3)
assert_eq(2 * 1.5, 3)
assert_eq(1.5 / 2, 0.75)

; f64 across function-call boundary (arg + return in XMM registers).
fn id(f64 x) f64 { x }

fn add2(f64 x, f64 y) f64 { x + y }
assert_eq(id(1.5), 1.5)
assert_eq(id(2.5) > id(1.0), true)
assert_eq(add2(1.5, 2.5), 4)
assert_eq(add2(1.0, 2.0), 3)

; f32 comparisons and arithmetic
assert_eq(1.5f32 < 1.0f32, false)
assert_eq(1.0f32 < 1.5f32, true)
assert_eq(1.5f32 * 2, 3)
print("native f64 compare/mixed/call ok")
