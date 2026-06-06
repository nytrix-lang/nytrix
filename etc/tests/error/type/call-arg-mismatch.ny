;; expect: cannot assign string literal to int
use std.core

fn need(int x) int {
   x
}

print(need("x"))
