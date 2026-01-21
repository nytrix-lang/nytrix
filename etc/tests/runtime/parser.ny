use std.io

; Test parser edge cases and complex expressions

; Nested function calls
def result1 = len(append([1, 2], 3))
assert(result1 == 3, "nested function calls")

; Complex arithmetic
def result2 = (10 + 5) * 2 - 8 / 4
assert(result2 == 28, "complex arithmetic")

; Nested parentheses
def result3 = ((5 + 3) * (10 - 2)) / 4
assert(result3 == 16, "nested parentheses")

; Chained comparisons
def x = 5
assert(x > 0, "x > 0")
assert(x < 10, "x < 10")
assert(x >= 5, "x >= 5")
assert(x <= 5, "x <= 5")
assert(x == 5, "x == 5")
assert(x != 4, "x != 4")

; Modulo operator in expressions
def mod_result = 17 % 5
assert(mod_result == 2, "modulo operator")

; If statements without parentheses
if x % 2 == 1 {
   print("x is odd")
}

; List operations
def lst = [1, 2, 3, 4, 5]
assert(len(lst) == 5, "list length")
assert(lst[0] == 1, "list index 0")
assert(lst[4] == 5, "list index 4")

; Dict operations
def dict = {"a": 1, "b": 2, "c": 3}
assert(dict["a"] == 1, "dict access")
assert(dict["c"] == 3, "dict access 2")

; Anonymous functions in expressions
fn square(x) { return x * x }
def sq_result = square(7)
assert(sq_result == 49, "function call in expression")

; If/else blocks
def ternary_test = 42
def ternary_result = 0
if ternary_test > 40 {
   ternary_result = 1
} else {
   ternary_result = 0
}
assert(ternary_result == 1, "if-else expression")

print("✓ Parser tests passed")

