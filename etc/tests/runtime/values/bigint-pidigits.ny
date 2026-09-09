use std.core
use std.math.big

;; Keep direct BigInt entry points heap-backed for small operands, then exercise
;; one-limb arithmetic in a pidigits-shaped accumulation loop.
def small_sum = __bigint_add(1, 2)
def small_product = __bigint_mul(3, 4)
assert(__tagof(small_sum) == __runtime_tag("bigint"), "small add remains BigInt")
assert(__tagof(small_product) == __runtime_tag("bigint"), "small mul remains BigInt")
assert(__bigint_to_str(small_sum) == "3", "small add value")
assert(__bigint_to_str(small_product) == "12", "small mul value")
def max_small = bigint_from_str("4611686018427387903")
def min_small = bigint_from_str("-4611686018427387904")
def promoted_add = __bigint_add(max_small, 1)
def promoted_mul = __bigint_mul(min_small, 2)
assert(__tagof(promoted_add) == __runtime_tag("bigint"), "add overflow promotion")
assert(__tagof(promoted_mul) == __runtime_tag("bigint"), "mul overflow promotion")
assert(__bigint_to_str(promoted_add) == "4611686018427387904", "add overflow value")
assert(__bigint_to_str(promoted_mul) == "-9223372036854775808", "mul overflow value")
mut acc = __bigint_from_int(0)
mut term = __bigint_from_int(1)
mut i = 0
while i < 32 {
   acc = __bigint_add(acc, term)
   term = __bigint_mul(term, __bigint_from_int(3))
   i += 1
}

assert(__bigint_to_str(acc) == "926510094425920", "pidigits-shaped accumulation")
print("bigint pidigits fast path passed")
