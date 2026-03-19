use std.core
use std.core.reflect
use std.math.nt
use std.math.big

;; BigInt regressions:
;; - printing must show decimal value (not "0")
;; - type/to_str/eq must work for BigInt values created via different entry points
def n = 12345

;; Construction paths
def x1 = __bigint_from_int(n)
def x2 = bigint_from_int(n)
def x3 = nt_bigint(n)
def x4 = Z(n)
def bigint_tag = __runtime_tag("bigint")
assert(__tagof(x1) == bigint_tag, "__bigint_from_int tag")
assert(__tagof(x2) == bigint_tag, "bigint_from_int tag")
assert(__tagof(x3) == bigint_tag, "nt_bigint tag")
assert(__tagof(x4) == bigint_tag, "Z tag")
assert(__bigint_to_str(x1) == "12345", "__bigint_to_str from int")
assert(__bigint_to_str(x2) == "12345", "__bigint_to_str bigint_from_int")
assert(__bigint_to_str(x4) == "12345", "__bigint_to_str Z")

;; reflect/type conversions
assert(type(x4) == "bigint", "type(Z(12345))")
assert(to_str(x4) == "12345", "to_str(Z(12345))")

;; equality paths
assert(bigint_eq(Z(2), Z(2)), "bigint_eq basic")
assert(Z(2) == Z(2), "== bigint basic")
assert(Z(2) != Z(3), "!= bigint basic")

;; arithmetic sanity
def mul = bigint_mul(Z(3), Z(4))
assert(to_str(mul) == "12", "bigint_mul + to_str")
assert(bigint_to_str(mul) == "12", "bigint_to_str")

;; nth_root sanity
def r8 = nth_root(Z(8), 3)
assert(r8 != nil, "nth_root returns")
assert(r8 == Z(2), "nth_root cbrt(8)")

;; BigInt sugar regressions
def sugar_x = Z(21)
assert(sugar_x.str == "21", "inferred bigint property str")
assert(sugar_x.bits == 5, "inferred bigint property bits")
assert(Z(2).pow_int(8).str == "256", "bigint method on constructor")
assert(Z(5).xor(Z(2)).str == "7", "bigint xor method")
assert((Z(2)^Z(8)).str == "256", "bigint power operator")
assert((Z(5)^^Z(2)).str == "7", "bigint xor operator")
assert((5^^Z(2)).str == "7", "int bigint xor operator")
assert(Z(4).sqrt_mod(17).powmod(2, 17) == Z(4), "bigint modular sqrt method")
def qroots = Z(1).quadratic_roots_mod(0, -4, 17)
assert(qroots.len == 2, "bigint quadratic roots method")
assert(bigint_xor(Z(255), Z(2)).str == "253", "bigint xor helper")
assert((Z(8) + Z(13)).str == "21", "bigint operator sugar")
assert(bigint_add(bigint_mul(Z(1), Z(16)), Z(5)).str == "21", "nt reexported bigint helpers")

;; .long and byte conversion regressions
static_assert([1, 2, 3].long == 0x010203, "static list .long")
static_assert("ABC".long == 0x414243, "static string .long")
static_assert(123.long == 123, "static int .long")
assert([1, 2, 3].long == Z(0x010203), "list .long")
assert("ABC".long == Z(0x414243), "string .long")
assert(123.long == Z(123), "int .long")
assert(1.9.long == Z(1), "float .long")
def list_long_bytes = [1, 2, 3].long.bytes
assert(type_shape(list_long_bytes) == "list<int>", ".long.bytes keeps typed byte list")
assert(list_long_bytes == [1, 2, 3], ".long.bytes roundtrip")
assert("010203".unhex.long.bytes == [1, 2, 3], ".unhex.long.bytes roundtrip")
assert([1, 2, 3].long.as_bytes.long.bytes == [1, 2, 3], "bigint bytes aliases chain")
print("✓ bigint tests passed")
