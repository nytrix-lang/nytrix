;; flags: --borrow-check
;; expect: cannot release owned slot 'a' while borrow 'b' is live
use std.core

fn bad(): int {
   def a = [1]
   def b = &a
   release(a)
   len(b)
}

bad()
