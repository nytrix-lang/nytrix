;; expect: cannot assign string literal to int
use std.core

fn need_int(int x) int {
   x
}

need_int("hi")
