;; flags: --borrow-check
;; expect: returning borrow of local owner 'a' would outlive its slot
use std.core

@borrows(x)
@returns_borrow(x)
fn peek(x) { x }

fn bad() {
   def a = [1]
   return peek(a)
}

bad()
