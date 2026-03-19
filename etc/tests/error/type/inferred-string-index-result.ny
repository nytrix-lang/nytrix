;; expect: cannot assign string literal to int
use std.core

fn first(xs){
   xs[0]
}

def int: c = first("AZ")
print(c)
