;; flags: --borrow-check
;; expect: double release of owned slot 'a'
use std.core

fn bad(): int {
   def a = [1]
   release(a)
   release(a)
   0
}

bad()
