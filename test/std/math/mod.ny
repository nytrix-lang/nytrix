use std.io
; Test std.math.mod - Mathematical operations
use std.math

; Basic math
assert(abs(-5) == 5, "abs negative")
assert(abs(5) == 5, "abs positive")
assert(abs(0) == 0, "abs zero")
assert(min(3, 7) == 3, "min")
assert(min(7, 3) == 3, "min reversed")
assert(max(3, 7) == 7, "max")
assert(max(7, 3) == 7, "max reversed")

; Power/Sqrt
assert(pow(2, 3) == 8, "2^3")
assert(pow(5, 2) == 25, "5^2")
assert(pow(10, 0) == 1, "10^0")
assert(pow(2, 10) == 1024, "2^10")
assert(sqrt(16) == 4, "sqrt(16)")
assert(sqrt(25) == 5, "sqrt(25)")
assert(sqrt(1) == 1, "sqrt(1)")
assert(sqrt(0) == 0, "sqrt(0)")

; Floor/Ceil
assert(floor(3) == 3, "floor positive")
assert(floor(-3) == -3, "floor negative")
assert(ceil(3) == 3, "ceil positive")
assert(ceil(-3) == -3, "ceil negative")
print("✓ std.math.mod tests passed")
