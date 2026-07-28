use std.core
use std.math.big (bigint, bigint_add, bigint_sub, bigint_from_str)

; ============================================================
; Int boundary-value characterization tests
;
; Purpose: verify that integer operations work correctly at
; the edges of both the tagged (62-bit) range and the full
; i64 range.  These are regression guards for the two-world
; representation refactor.
; ============================================================

; --- Tagged-integer small-int range (63-bit signed) ---
def SMALL_MAX =  2305843009213693951  ; 2^61 - 1
def SMALL_MIN = -2305843009213693952  ; -(2^61)

assert(__is_int(SMALL_MAX), "SMALL_MAX is a tagged int")
assert(__is_int(SMALL_MIN), "SMALL_MIN is a tagged int")
assert_eq(SMALL_MAX + 1, __bigint_to_int(bigint_add(bigint(SMALL_MAX), bigint(1))), "SMALL_MAX + 1 promotes to bigint")
assert_eq(SMALL_MIN - 1, __bigint_to_int(bigint_sub(bigint(SMALL_MIN), bigint(1))), "SMALL_MIN - 1 promotes to bigint")

; --- Arithmetic at tagged range edges ---
assert_eq(SMALL_MAX - SMALL_MAX, 0, "SMALL_MAX - SMALL_MAX == 0")
assert_eq(SMALL_MIN - SMALL_MIN, 0, "SMALL_MIN - SMALL_MIN == 0")

; --- Zero and one ---
assert_eq(0 + 0, 0, "0 + 0 == 0")
assert_eq(1 - 1, 0, "1 - 1 == 0")
assert_eq(-1 + 1, 0, "-1 + 1 == 0")
assert_eq(0 * 1000000, 0, "0 * large == 0")

; --- Signed overflow via bigint ---
def i64_max = bigint_from_str("9223372036854775807")   ; 2^63 - 1
def i64_min = bigint_from_str("-9223372036854775808")  ; -(2^63)
def one = bigint(1)

assert_eq(to_str(i64_max), "9223372036854775807", "i64_max to_str")
assert_eq(to_str(i64_min), "-9223372036854775808", "i64_min to_str")
assert_eq(to_str(__bigint_to_int(i64_max)), "9223372036854775807", "i64_max bigint-to-int preserves value")
assert_eq(to_str(__bigint_to_int(i64_min)), "-9223372036854775808", "i64_min bigint-to-int preserves value")
assert_eq(to_str(bigint_add(i64_max, one)), "9223372036854775808", "i64_max + 1 bigint")
assert_eq(to_str(bigint_sub(i64_min, one)), "-9223372036854775809", "i64_min - 1 bigint")

; --- Powers of two at boundary ---
assert_eq(1 << 30, 1073741824, "2^30 shift")
assert_eq(1 << 60, 1152921504606846976, "2^60 shift")
; 2^62 exceeds SMALL_MAX, so it promotes
def two_62 = bigint_add(bigint(1) << 62, bigint(0))
assert_eq(to_str(two_62), "4611686018427387904", "2^62 via bigint")

; --- Division rounding ---
assert_eq(7 / 2, 3, "7/2 floors toward zero")
assert_eq(-7 / 2, -3, "-7/2 floors toward zero")
assert_eq(7 % 2, 1, "7 % 2 == 1")
assert_eq(-7 % 2, -1, "-7 % 2 == -1")

; --- Bitwise at boundaries ---
assert_eq(SMALL_MAX & SMALL_MAX, SMALL_MAX, "SMALL_MAX & SMALL_MAX")
assert_eq(0 | SMALL_MAX, SMALL_MAX, "0 | SMALL_MAX")
assert_eq(SMALL_MAX ^^ SMALL_MAX, 0, "SMALL_MAX ^^ SMALL_MAX == 0")
assert_eq(~0, -1, "~0 == -1")

; --- Float boundary values ---
def f_max = 1.7976931348623157e+308
def f_min = 1e-307
assert(is_float(f_max), "float max is a float")
assert(is_float(f_min), "float min (denorm) is a float")
assert(f_max > 0.0, "float max > 0")
assert(f_min > 0.0, "float min > 0")
assert(f_max > f_min, "float max > float min")

; --- Int/float boundary conversion ---
def large = 1000000000000000
assert(__is_int(large), "1e15 is tagged int")
def flarge = float(large)
assert(is_float(flarge), "float(1e15) is float")
assert(flarge > 9.9e14, "float(1e15) > 9.9e14")

print("✓ int boundary tests passed")
