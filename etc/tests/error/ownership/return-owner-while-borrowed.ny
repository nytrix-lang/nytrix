;; flags: --borrow-check
;; expect: cannot return owned slot 'a' while borrow 'b' is live
use std.core

fn bad() {
   def a = [1]
   def b = borrow(a)
   return a
}

bad()
