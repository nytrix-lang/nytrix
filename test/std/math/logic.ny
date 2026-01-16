use std.io
; Test std.math.logic - Logical and number theory operations
use std.math.logic

; GCD LCM
assert(gcd(12, 8) == 4, "gcd(12, 8)")
assert(gcd(17, 19) == 1, "gcd coprime")
assert(gcd(100, 50) == 50, "gcd with factor")
assert(lcm(4, 6) == 12, "lcm(4, 6)")
assert(lcm(3, 5) == 15, "lcm coprime")

; Factorial
assert(factorial(0) == 1, "0!")
assert(factorial(1) == 1, "1!")
assert(factorial(5) == 120, "5!")
assert(factorial(6) == 720, "6!")
print("✓ std.math.logic tests passed")
