use std.io
use std.math.float
use std.math.stat
use std.math.nt
use std.os.time
use std.core.test
use std.collections
use std.iter

print("Testing Math Extras...")
; Float
assert(is_nan(nan()), "nan")
assert(is_inf(inf()), "inf")

; Stat
print("Testing Stat...")
def d = list()
d = append(d, 1)
d = append(d, 2)
d = append(d, 3)
d = append(d, 4)
d = append(d, 5)
assert(sum(d) == 15, "sum")
assert(mean(d) == 3, "mean")

; Number Theory
print("Testing NT...")
assert(gcd(12, 18) == 6, "gcd")
assert(lcm(12, 18) == 36, "lcm")
assert(is_prime(7) == 1, "prime 7")
assert(is_prime(10) == 0, "prime 10")

; Time
print("Testing Time...")
def t = time()
print("Time:", t)
assert(t > 0, "time > 0")

print("✓ std.math.more passed")
