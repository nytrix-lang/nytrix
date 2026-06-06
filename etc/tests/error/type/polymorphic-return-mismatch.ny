;; expect: variable declaration: expected str, got int
use std.core

fn id(x) {
   x
}

def str: s = id(1)
print(s)
