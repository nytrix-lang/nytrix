;; expect: cannot assign string literal to int
use std.core

fn loop_body_is_not_return() int {
   mut i = 0
   while i < 1 {
      "not the return"
      i += 1
   }
   return "real mismatch"
}

print(loop_body_is_not_return())
