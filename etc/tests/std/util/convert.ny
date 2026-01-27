use std.io
use std.util.convert
use std.strings.str
use std.core.error

;; std.util.convert (Test)
;; Tests string, int, and bool conversions.

print("Testing Util Convert...")

def s123 = to_str(123)
assert(eq(s123, "123"), "to_str int")
assert(eq(to_str(true), "true"), "to_str bool")
assert(eq(to_str("s"), "s"), "to_str str")

assert(eq(int_to_str(123), "123"), "int_to_str")
assert(eq(int_to_str(-456), "-456"), "int_to_str neg")

assert(parse_int("123") == 123, "parse_int")
assert(parse_int("-123") == -123, "parse_int neg")
assert(parse_int("0") == 0, "parse_int zero")

assert(to_bool(1) == true, "to_bool 1")
assert(to_bool(0) == false, "to_bool 0")
assert(to_bool("a") == true, "to_bool str")
assert(to_bool("") == false, "to_bool empty")

print("✓ std.util.convert tests passed")
