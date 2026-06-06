;; flags: --borrow-check
;; expect: cannot forget owned slot 'a' while borrow 'b' is live
use std.core

fn bad() int {
   def a = [1]
   def b = borrow(a)
   forget(a)
   len(b)
}

bad()
