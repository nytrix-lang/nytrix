;; flags: --borrow-check
;; expect: cannot reassign owned slot 'a' while borrow 'b' is live
use std.core

fn bad(): int {
   mut a = [1]
   def b = borrow(a)
   a = [2]
   len(b)
}

bad()
