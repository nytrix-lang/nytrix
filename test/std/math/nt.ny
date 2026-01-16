use std.io
use std.math.nt
use std.core.test
use std.core
use std.strings.str

print("Testing Math Number Theory...")

def res = egcd(10, 6) ; g=2, x=-1, y=2? 10(-1) + 6(2) = -10 + 12 = 2.
assert(get(res, 0) == 2, "egcd gcd")
assert(get(res, 1) * 10 + get(res, 2) * 6 == 2, "egcd valid")

assert(modinv(3, 11) == 4, "modinv 3 11") ; 3*4 = 12 = 1 mod 11.
assert(modinv(2, 6) == 0, "modinv 2 6") ; no inverse

assert(pow_mod(2, 10, 1000) == 24, "pow_mod") ; 1024 % 1000 = 24.
assert(pow_mod(2, 3, 5) == 3, "pow_mod small")

assert(is_prime(2) == 1, "prime 2")
assert(is_prime(3) == 1, "prime 3")
assert(is_prime(4) == 0, "prime 4")
assert(is_prime(97) == 1, "prime 97")
assert(is_prime(100) == 0, "prime 100")

assert(next_prime(10) == 11, "next_prime 10")

print("✓ std.math.nt passed")
