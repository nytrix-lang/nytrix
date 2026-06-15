;; expect: cannot assign string literal to int
use std.core

fn choose(flag, a, b) {
   if flag { return a }
   b
}

def int x = choose(true, 1, "x")
print(x)
