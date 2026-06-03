use std.core
use std.core.error
use std.core.reflect
use std.core.dict
use std.core.io
use std.core.str

def p = malloc(10)
assert(p != 0, "malloc")
store8(p, 65, 0)
store8(p, 66, 1)
assert(load8(p, 0) == 65, "load8 0")
assert(load8(p, 1) == 66, "load8 1")
def offset = 1
assert(load8(p, offset) == 66, "load8 var")
free(p)
mut a = 10
mut b = 20
assert(a + b == 30, "add")
assert(b - a == 10, "sub")
assert(a * b == 200, "mul")
assert(b / a == 2, "div")
assert(b % 3 == 2, "mod")
assert_eq(2^20, 1048576, "power operator")
assert_eq(2^^20, 22, "xor operator")
assert_eq(2^3^2, 512, "power right associativity")
assert_eq(-2^2, -4, "power binds before unary minus")
assert_eq((-2)^2, 4, "parenthesized negative power")
assert_eq(2^-1, 0.5, "negative exponent")
def big_lit = 4611686018427387904
assert(type(big_lit) == "bigint", "large int literal promotes to bigint")
assert(to_str(big_lit) == "4611686018427387904", "large int literal value")
def big_add = 4611686018427387903 + 1
def big_sub_base = -4611686018427387904
def big_sub = big_sub_base - 1
def big_mul = 4611686018427387903 * 2
assert(type(big_add) == "bigint", "add overflow promotes to bigint")
assert(type(big_sub_base) == "bigint", "negative boundary lowers to bigint")
assert(type(big_sub) == "bigint", "sub overflow promotes to bigint")
assert(type(big_mul) == "bigint", "mul overflow promotes to bigint")
assert(to_str(big_add) == "4611686018427387904", "add overflow value")
assert(to_str(big_sub) == "-4611686018427387905", "sub overflow value")
assert(to_str(big_mul) == "9223372036854775806", "mul overflow value")

fn fib(n){
   if(n < 2){ return n }
   fib(n - 1) + fib(n - 2)
}

assert(fib(10) == 55, "fib")

fn adder(x){
   lambda(y){ x + y }
}

def add5 = adder(5)
def add10 = adder(10)
assert(add5(10) == 15, "closure 5")
assert(add10(10) == 20, "closure 10")
mut x = 42
del x
assert(x == nil, "del")
mut y = nil
assert(y == nil, "nil")
mut xs = [3, 1, 2]
assert(sort(xs) == [1, 2, 3], "sort list result")
assert(xs == [1, 2, 3], "sort mutates list")
assert(sorted([3, 1, 2]) == [1, 2, 3], "sorted list copy")
assert(sorted([]) == [], "sorted empty list")
assert(sorted([7]) == [7], "sorted single list")
assert(sqrt(9) == 3.0, "core sqrt")
assert(swapped([1, 2, 3], 0, 2) == [3, 2, 1], "swapped list copy")
assert(swapped([], 0, 0) == [], "swapped empty list")
assert(swapped([7], 0, 0) == [7], "swapped single list")
mut idx_swap = [1, 2, 3, 4]
mut si = 0
mut sj = idx_swap.len - 1
while(si < sj){
   def tmp = idx_swap[si]
   idx_swap[si] = idx_swap[sj]
   idx_swap[sj] = tmp
   si += 1
   sj -= 1
}

assert(idx_swap == [4, 3, 2, 1], "index assignment swap")
assert(clone([1, 2, 3]) == [1, 2, 3], "clone list")
assert(clone("ab") == "ab", "clone string")
assert(clone((1, 2)) == (1, 2), "clone tuple")
assert(clear("ab") == "", "clear string")
assert(clear((1, 2)) == (), "clear tuple")
add(xs, 4)
assert(xs == [1, 2, 3, 4], "add mutates list in statement position")
clear(xs)
assert(xs == [], "clear list")
mut d = dict()
d = d.set("a", 10)
assert(d.get("a", 0) == 10, "get dict")
assert(d.contains("a"), "contains dict key")
assert(keys(d) == ["a"], "dict keys")
assert(values(d) == [10], "dict values")
assert(items(d) == [["a", 10]], "dict items")
d = d.delete("a")
assert(!d.contains("a"), "delete dict key")
d = d.set("b", 11)
clear(d)
assert(!d.contains("b"), "clear dict")
assert(keys(d) == [], "clear dict keys")
mut half_full = dict(20)
mut fill_i = 0
while(fill_i < 16){
   half_full = half_full.set("k" + to_str(fill_i), fill_i)
   fill_i += 1
}
fn update_half_full_dict(any: x): int {
   x.set("k0", 99)
   0
}
update_half_full_dict(half_full)
assert(half_full.get("k0", 0) == 99, "dict update at resize threshold mutates existing key")
mut s = set()
s = add(s, "x")
s = add(s, "y")
assert(s.contains("x"), "contains set value")
s = sub(s, "x")
assert(!s.contains("x"), "remove set value")
s = clear(s)
assert(!s.contains("y"), "clear set")
print("✓ std.core runtime tests passed")
