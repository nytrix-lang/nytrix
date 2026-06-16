use std.core
use std.core.error
use std.core.reflect
use std.core.dict
use std.core.io
use std.core.str

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
assert(lst.len == 5, "list len")
assert(lst[0] == 1, "list idx 0")
assert(lst[4] == 5, "list idx 4")
def d = {"a": 1, "b": 2, "c": 3}
assert(d["a"] == 1, "dict a")
assert(d["c"] == 3, "dict c")

fn dict_expr_direct() {
   {"x": 10, "y": 20}
}

fn dict_expr_local(v) {
   def out = {"value": v, "plus": v + 1}
   out
}

def d2 = dict_expr_direct()
def d3 = dict_expr_local(41)
assert(d2["x"] == 10 && d2["y"] == 20, "direct dict expr")
assert(d3["value"] == 41 && d3["plus"] == 42, "local dict expr")

fn square(x) { x * x }
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

;; REPL migration: long doc signature
fn repl_long_doc_signature(
   int p00, int p01, int p02, int p03, int p04, int p05,
   int p06, int p07, int p08, int p09, int p10, int p11,
   int p12, int p13, int p14, int p15, int p16, int p17,
   int p18, int p19, int p20, int p21, int p22, int p23,
   int p24, int p25, int p26, int p27, int p28, int p29,
   int p30, int p31, int p32, int p33, int p34, int p35,
   int p36, int p37, int p38, int p39, int p40, int p41,
   int p42, int p43, int p44, int p45, int p46, int p47,
   int p48, int p49, int p50, int p51, int p52, int p53,
   int p54, int p55, int p56, int p57, int p58, int p59
) int {
   "Forces REPL doc collection to retain a signature longer than the old fixed stack buffer."
   1
}

fn main() {
   assert(repl_long_doc_signature(
         0, 1, 2, 3, 4, 5,
         6, 7, 8, 9, 10, 11,
         12, 13, 14, 15, 16, 17,
         18, 19, 20, 21, 22, 23,
         24, 25, 26, 27, 28, 29,
         30, 31, 32, 33, 34, 35,
         36, 37, 38, 39, 40, 41,
         42, 43, 44, 45, 46, 47,
         48, 49, 50, 51, 52, 53,
         54, 55, 56, 57, 58, 59
   ) == 1, "long REPL doc signature survives")
}
main()
