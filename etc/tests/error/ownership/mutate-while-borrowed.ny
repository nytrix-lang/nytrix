;; flags: --borrow-check
;; expect: cannot mutate owned slot 'a' while borrow 'b' is live
use std.core

@mutates(x)
fn touch(x): int {
   0
}

fn bad(): int {
   def a = [1, 2]
   def b = borrow(a)
   touch(a)
   len(b)
}

bad()
