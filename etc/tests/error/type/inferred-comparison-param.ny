;; expect: cannot assign string literal to int
use std.core

fn small(x){
   x < 10
}

print(small("x"))
