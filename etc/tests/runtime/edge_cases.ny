use std.io
use std.core.error

;; Edge cases and error handling (Test)

def r = 10 / 2
assert(r == 5, "division")

def undefined_var = 0
assert(undefined_var == 0, "default value")

def arr = [1, 2, 3]
assert(arr[0] == 1, "array access")

def empty_list = []
assert(len(empty_list) == 0, "empty list")

def nested = [[1, 2], [3, 4], [5, 6]]
assert(len(nested) == 3, "nested outer")
assert(len(nested[0]) == 2, "nested inner")
assert(nested[1][1] == 4, "nested access")

def mixed = [1, "two", 3.0]
assert(len(mixed) == 3, "mixed list")

def big_num = 999999999
assert(big_num > 0, "big pos")

def neg_num = -999999999
assert(neg_num < 0, "big neg")

def float_result = 3.14 + 2.86
assert(float_result >= 6.0, "float add low")
assert(float_result < 6.1, "float add high")

def bool_test = (1 == 1) and (2 < 3) and (5 > 4)
assert(bool_test, "bool expr")

fn no_args(){ 42 }
assert(no_args() == 42, "fn no args")

fn multi_args(a,b,c){ a + b + c }
assert(multi_args(1,2,3) == 6, "fn multi args")

fn factorial(n){
   if(n <= 1){ return 1 }
   n * factorial(n - 1)
}
assert(factorial(5) == 120, "factorial")
assert(factorial(0) == 1, "factorial base")

def outer = 10
fn scope_test(){
   def inner = 20
   inner + outer
}
assert(scope_test() == 30, "scope")

def shadow = 1
fn shadow_test(){
   def shadow = 2
   shadow
}
assert(shadow_test() == 2, "shadow inner")
assert(shadow == 1, "shadow outer")

print("✓ Edge case tests passed")
