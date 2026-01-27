use std.math.bigint *
use std.core.error *
use std.core.reflect *

;; std.math.bigint (Test)
;; Tests bigint arithmetic and sign handling.

print("Testing bigint basic...")
mut a = bigint_from_str("1")
mut b = bigint_from_str("999999999")
mut s = bigint_add(a, b)
assert(eq(bigint_to_str(s), "1000000000"), "add simple 1: got " + bigint_to_str(s))

a = bigint_from_str("1000000000")
b = bigint_from_str("1000000000")
s = bigint_add(a, b)
assert(eq(bigint_to_str(s), "2000000000"), "add simple 2")
def d = bigint_sub(bigint_from_str("1000000000000000000000000000000"), bigint_from_str("135802467913580246791358024680"))
assert(eq(bigint_to_str(d), "864197532086419753208641975320"), "sub")
def m = bigint_mul(bigint_from_str("123456789"), bigint_from_str("987654321"))
assert(eq(bigint_to_str(m), "121932631112635269"), "mul")
mut q = bigint_div(bigint_from_str("1000000000000"), bigint_from_str("12345"))
mut r = bigint_mod(bigint_from_str("1000000000000"), bigint_from_str("12345"))
assert(eq(bigint_to_str(q), "81004455"), "div")
assert(eq(bigint_to_str(r), "3025"), "mod")
print("bigint basic passed")

print("Testing bigint sign...")
a = bigint_from_str("-999999999999")
b = bigint_from_str("2")
s = bigint_add(a, b)
assert(eq(bigint_to_str(s), "-999999999997"), "add sign")
def p = bigint_mul(a, b)
assert(eq(bigint_to_str(p), "-1999999999998"), "mul sign")
q = bigint_div(a, b)
assert(eq(bigint_to_str(q), "-499999999999"), "div sign")
r = bigint_mod(a, b)
assert(eq(bigint_to_str(r), "-1"), "mod sign")
print("bigint sign passed")

print("✓ std.math.bigint tests passed")
