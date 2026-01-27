use std.math.nt *
use std.core.error *

;; std.math.nt (Test)
;; Tests extended gcd, modular inverse, modular exponentiation, and primes.

print("Testing Math Number Theory...")

def res = egcd(10, 6)
assert(get(res, 0) == 2, "egcd gcd")
assert(get(res, 1) * 10 + get(res, 2) * 6 == 2, "egcd valid")

assert(gcd(12, 18) == 6, "gcd")
assert(lcm(12, 18) == 36, "lcm")

assert(modinv(3, 11) == 4, "modinv 3 11")
assert(modinv(2, 6) == 0, "modinv 2 6")

assert(pow_mod(2, 10, 1000) == 24, "pow_mod")
assert(pow_mod(2, 3, 5) == 3, "pow_mod small")

assert(is_prime(2) == 1, "prime 2")
assert(is_prime(3) == 1, "prime 3")
assert(is_prime(4) == 0, "prime 4")
assert(is_prime(97) == 1, "prime 97")
assert(is_prime(100) == 0, "prime 100")

assert(next_prime(10) == 11, "next_prime")

print("✓ std.math.nt tests passed")
