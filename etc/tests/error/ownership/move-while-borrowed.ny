;; flags: --borrow-check
;; expect: cannot move owned slot 'a' while borrow 'b' is live
use std.core

@returns_owned
@consumes(x)
fn adopt(x){ x }

fn bad(): int {
   def a = [1, 2]
   def b = borrow(a)
   def c = adopt(a)
   len(b) + len(c)
}

bad()
