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

;; Increment / Decrement Tests
mut count = 5
++count
assert(count == 6, "++pre-increment")
--count
assert(count == 5, "--pre-decrement")

;; Increment in while loop header
mut loop_i = 0
mut loop_sum = 0
mut i=0
while(i<10 ++i){
   loop_sum = loop_sum + i
}
assert(loop_sum == 45, "loop sequence failed")

;; Compound Assignment Tests
mut val = 10
val += 5
assert(val == 15, "+= failed")
val -= 3
assert(val == 12, "-= failed")
val *= 2
assert(val == 24, "*= failed")
val /= 4
assert(val == 6, "/= failed")
val %= 4
assert(val == 2, "%= failed")

print("✓ Comprehensive operator tests passed")
