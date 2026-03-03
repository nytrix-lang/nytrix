use std.core
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *

;; std.core runtime primitives (Test)

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
undef x
assert(x == 0, "undef")

mut y = nil
assert(y == 0, "nil")

print("✓ std.core runtime tests passed")
