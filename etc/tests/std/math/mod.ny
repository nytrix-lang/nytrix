use std.math *
use std.core.error *

;; std.math.mod (Test)
;; Tests basic math, power, sqrt, floor, and ceil.

assert(abs(-5) == 5, "abs neg")
assert(abs(5) == 5, "abs pos")
assert(abs(0) == 0, "abs zero")

assert(min(3,7) == 3, "min")
assert(min(7,3) == 3, "min rev")
assert(max(3,7) == 7, "max")
assert(max(7,3) == 7, "max rev")

assert(pow(2,3) == 8, "pow 2^3")
assert(pow(5,2) == 25, "pow 5^2")
assert(pow(10,0) == 1, "pow 10^0")
assert(pow(2,10) == 1024, "pow 2^10")

assert(sqrt(16) == 4, "sqrt 16")
assert(sqrt(25) == 5, "sqrt 25")
assert(sqrt(1) == 1, "sqrt 1")
assert(sqrt(0) == 0, "sqrt 0")

assert(floor(3) == 3, "floor pos")
assert(floor(-3) == -3, "floor neg")
assert(ceil(3) == 3, "ceil pos")
assert(ceil(-3) == -3, "ceil neg")

print("✓ std.math.mod tests passed")
