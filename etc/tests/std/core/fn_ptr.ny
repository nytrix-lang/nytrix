use std.io
use std.core

;; Function Pointers (Test)
;; Tests direct function assignments, lambdas, and higher-order function calls.

def add_impl = lambda(a, b) { return a + b }
def sub_impl = lambda(a, b) { return a - b }

print("Testing basic function pointer...")
def f = add_impl
assert(f(10, 20) == 30, "direct fn assign call")
f = sub_impl
assert(f(10, 20) == -10, "reassigned fn call")

print("Testing lambda as pointer...")
def l = lambda(x, y) { return x * y }
assert(l(3, 4) == 12, "lambda call")
def l2 = lambda(x) { return x + 1 }
assert(l2(10) == 11, "fn expr call")

def apply_op = lambda(a, b, op) {
   def res = op(a, b)
   return res
}

print("Testing higher-order functions...")
assert(apply_op(5, 3, add_impl) == 8, "passed fn ptr")
assert(apply_op(5, 3, sub_impl) == 2, "passed fn ptr 2")
assert(apply_op(5, 3, lambda(a, b){ return a * b }) == 15, "passed lambda")

print("✓ std.core.fn_ptr tests passed")
