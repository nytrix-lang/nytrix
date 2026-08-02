use std.core
use std.core.error
use std.core.reflect
use std.core.io

; Arithmetic
assert_eq(1 + 2, 3)
assert_eq(5 - 3, 2)
assert_eq(3 * 4, 12)
assert_eq(10 / 3, 3)
assert_eq(10 % 3, 1)
assert_eq(-7, -7)
assert_eq(-(5 - 3), -2)

; Bitwise
assert_eq(6 & 3, 2)
assert_eq(6 | 3, 7)
assert_eq(6 ^^ 3, 5)
assert_eq(3 ^^ 3, 0)
assert_eq(6 ^^ 6, 0)
assert_eq(1 << 3, 8)
assert_eq(16 >> 2, 4)
assert_eq(~0, -1)
assert_eq(~1, -2)

; POW (^) is exponentiation
assert_eq(6 ^ 3, 216)
assert_eq(2 ^ 8, 256)
assert_eq(10 ^ 0, 1)

; POW with a variable base must lower through std.core.pow (bigint), not the
; tagged-int fast path — this is the regression that once returned ~4.7e13
; because the result was wrongly treated as a tagged small int.
def int base = 3
def int exp = 2
assert_eq(base ^ exp, 9)
assert_eq(base ^ 2, 9)
assert_eq(2 ^ exp, 4)
fn ipow(int x) int { return x ^ 2 }
assert_eq(ipow(2), 4)
assert_eq(ipow(4), 16)
assert_eq(ipow(5), 25)

; Comparison
assert_eq(3 < 5, true)
assert_eq(5 < 3, false)
assert_eq(3 <= 3, true)
assert_eq(3 > 5, false)
assert_eq(5 > 3, true)
assert_eq(5 >= 5, true)
assert_eq(5 == 5, true)
assert_eq(5 != 3, true)
assert_eq(5 == 3, false)

print("✓ native arithmetic tests passed")
