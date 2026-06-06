;; flags: --borrow-check
;; expect: use after move of owned slot 'a'
use std.core

@consumes(x)
fn take(x) int {
   0
}

fn bad() int {
   def a = [1]
   take(a)
   len(a)
}

bad()
