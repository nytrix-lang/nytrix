use std.core
use std.core.error
use std.core.reflect
use std.core.io

; Boolean literals
assert_eq(true, true)
assert_eq(false, false)
assert_eq(!true, false)
assert_eq(!false, true)
assert_eq(true && true, true)
assert_eq(true && false, false)
assert_eq(false || true, true)
assert_eq(false || false, false)

; Negation of comparisons
fn not_gt(i64 a, i64 b) bool { !(a > b) }
assert_eq(not_gt(3, 5), true)
assert_eq(not_gt(5, 3), false)

; Large constants
assert_eq(2147483647, 2147483647)
assert_eq(-2147483648, -2147483648)
assert_eq(0x7FFFFFFFFFFFFFFF, 9223372036854775807)

; Zero and negative constants
assert_eq(0, 0)
assert_eq(-0, 0)
assert_eq(-1, -1)

; Constant folding with mixed operations
fn complex_const() i64 { (2 + 3) * (10 - 4) / 3 }
assert_eq(complex_const(), 10)

print("✓ native boolean tests passed")
