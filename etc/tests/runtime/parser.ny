use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *

;; Parser edge cases and complex expressions (Test)

mut result1 = len(append([1, 2], 3))
assert(result1 == 3, "nested calls")

mut result2 = (10 + 5) * 2 - 8 / 4
assert(result2 == 28, "complex arithmetic")

mut result3 = ((5 + 3) * (10 - 2)) / 4
assert(result3 == 16, "nested parentheses")

mut x = 5
assert(x > 0, "gt")
assert(x < 10, "lt")
assert(x >= 5, "gte")
assert(x <= 5, "lte")
assert(x == 5, "eq")
assert(x != 4, "neq")

mut mod_result = 17 % 5
assert(mod_result == 2, "mod")

if x % 2 == 1 {
   print("x is odd")
}

def lst = [1, 2, 3, 4, 5]
assert(len(lst) == 5, "list len")
assert(lst[0] == 1, "list idx 0")
assert(lst[4] == 5, "list idx 4")

def d = {"a": 1, "b": 2, "c": 3}
assert(d["a"] == 1, "dict a")
assert(d["c"] == 3, "dict c")

fn square(x){ x * x }
assert(square(7) == 49, "fn expr")

def ternary_test = 42
mut ternary_result = 0
if ternary_test > 40 {
   ternary_result = 1
} else {
   ternary_result = 0
}
assert(ternary_result == 1, "if else")

print("✓ Parser tests passed")
