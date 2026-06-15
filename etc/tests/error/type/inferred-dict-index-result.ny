;; expect: variable declaration: expected str, got int
use std.core

fn getx(d) {
   d["x"]
}

def str s = getx({"x": 1})
print(s)
