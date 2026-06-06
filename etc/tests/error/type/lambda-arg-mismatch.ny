;; expect: cannot assign string literal to int
use std.core

def f = fn(int x) int {
   x + 1
}

print(f("x"))
