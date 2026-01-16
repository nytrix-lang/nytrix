use std.io
use std.util.convert
use std.core.reflect
use std.core.test
use std.core
use std.strings.str

print("Testing Util Convert...")

def s123 = str(123)
print("str(123):", s123)
assert(eq(s123, "123"), "str(123)")
assert(eq(str(true), "true"), "str(true)")
assert(eq(str("s"), "s"), "str(s)")

def s_123 = int_to_str(123)
print("int_to_str(123):", s_123)
assert(eq(s_123, "123"), "int_to_str")

def s_neg = int_to_str(-456)
print("int_to_str(-456):", s_neg)
assert(eq(s_neg, "-456"), "int_to_str neg")

assert(parse_int("123") == 123, "parse_int")
assert(parse_int("-123") == -123, "parse_int neg")
assert(parse_int("0") == 0, "parse_int 0")

assert(to_bool(1) == true, "to_bool 1")
assert(to_bool(0) == false, "to_bool 0")
assert(to_bool("a") == true, "to_bool str")
assert(to_bool("") == false, "to_bool empty")

print("✓ std.util.convert passed")
