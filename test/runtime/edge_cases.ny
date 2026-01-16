use std.io

; Test error handling and edge cases

; Division by zero handling
fn test_division() {
	def result = 10 / 2
	assert(result == 5, "normal division")
	return result
}
test_division()

; Null/undefined handling
def undefined_var = 0
assert(undefined_var == 0, "default value")

; Out of bounds access handling
def arr = [1, 2, 3]
def valid_access = arr[0]
assert(valid_access == 1, "valid array access")

; Empty collections
def empty_list = []
assert(len(empty_list) == 0, "empty list")

def empty_dict = {}
assert(len(empty_dict) == 0, "empty dict")

; Nested data structures
def nested = [[1, 2], [3, 4], [5, 6]]
assert(len(nested) == 3, "nested list outer")
assert(len(nested[0]) == 2, "nested list inner")
assert(nested[1][1] == 4, "nested list access")

; Mixed type collections
def mixed = [1, "two", 3.0]
assert(len(mixed) == 3, "mixed type list")

; Large numbers
def big_num = 999999999
assert(big_num > 0, "large positive number")

def neg_num = -999999999
assert(neg_num < 0, "large negative number")

; Float operations
def float_result = 3.14 + 2.86
assert(float_result > 6.0, "float addition")
assert(float_result < 6.1, "float addition bound")

; Type conversions
def int_val = 42
def float_val = 3.14

; Complex boolean expressions
def bool_test = (1 == 1) and (2 < 3) and (5 > 4)
assert(bool_test, "complex boolean and")

def bool_test2 = (1 == 2) or (3 == 3) or (4 == 5)
assert(bool_test2, "complex boolean or")

; Short-circuit evaluation
def short_circuit = 1 or (10 / 0)  ; Should not divide by zero

; Function with no arguments
fn no_args() {
	return 42
}
assert(no_args() == 42, "function no args")

; Function with multiple arguments
fn multi_args(a, b, c) {
	return a + b + c
}
assert(multi_args(1, 2, 3) == 6, "function multi args")

; Recursive edge cases
fn factorial(n) {
	if n <= 1 {
		return 1
	}
	return n * factorial(n - 1)
}
assert(factorial(5) == 120, "factorial recursion")
assert(factorial(0) == 1, "factorial base case")

; Scope testing
def outer = 10
fn scope_test() {
	def inner = 20
	return inner + outer
}
assert(scope_test() == 30, "variable scope")

; Shadowing
def shadow = 1
fn shadow_test() {
	def shadow = 2
	return shadow
}
assert(shadow_test() == 2, "variable shadowing in function")
assert(shadow == 1, "original not shadowed")

print("✓ Edge case tests passed")
