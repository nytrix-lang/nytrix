use std.core
use std.io
use std.core.error

;; std.core runtime primitives (Test)

def p = __malloc(10)
assert(p != 0, "malloc")
__store8_idx(p, 0, 65)
__store8_idx(p, 1, 66)
assert(__load8_idx(p, 0) == 65, "load8 0")
assert(__load8_idx(p, 1) == 66, "load8 1")
def off = 1
assert(__load8_idx(p, off) == 66, "load8 var")
__free(p)

def a = 10
def b = 20
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

def x = 42
undef x
assert(x == 0, "undef")

def y = nil
assert(y == 0, "nil")

print("✓ std.core runtime tests passed")
