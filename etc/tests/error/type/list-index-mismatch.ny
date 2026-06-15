;; expect: variable declaration: expected str, got int
use std.core

def xs = [1, 2, 3]
def str s = xs[0]
print(s)
