;; flags: --safe-mode
;; expect: returning owned slot 'a' requires @returns_owned
use std.core

fn bad() {
   def a = [1]
   return a
}

bad()
