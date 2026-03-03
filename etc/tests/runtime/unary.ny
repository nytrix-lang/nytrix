use std.core *
use std.core.error *
use std.core.reflect *
use std.str.io *

;; Unary Operator Tests

def a = 10
def b = -a
assert(b == -10, "unary negation variable")

def c = -5
assert(c == -5, "unary negation literal")

def d = ~0
assert(d == -1, "bitwise not zero")

def e = ~1
assert(e == -2, "bitwise not one")

def f = 0
assert(-f == 0, "unary negation zero")

mut x = 100
x = -x
assert(x == -100, "unary negation mutable")

mut y = 10
y = ~y
assert(y == -11, "bitwise not mutable")

;; Large numbers (within 63-bit range)
def big = 9223372036854775807
def big_neg = -big
assert(big_neg == -9223372036854775807, "unary negation max int")

;; Additional refactor tests
assert(-5 == -5, "negation of positive")
assert(-(-5) == 5, "negation of negative")
assert(~0 == -1, "bitwise not of 0")
assert(~(-1) == 0, "bitwise not of -1")

def v_a = 10
def v_b = -v_a
assert(v_b == -10, "var negation")

def v_c = ~v_a
assert(v_c == -11, "var bitwise not")

print("✓ Unary operator tests passed")
