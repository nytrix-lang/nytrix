;; expect: variable declaration: expected str, got int
use std.core

fn first(xs) {
   xs[0]
}

def str s = first([1, 2])
print(s)
