;; expect: cannot assign string literal to int
use std.core

fn add1(x) {
   x + 1
}

print(add1("x"))
