;; repl-expect: REPL_LAZY_STD_ROOT_OK
use std

fn main(): int {
   assert(abs(10) == 10, "bare std root lazily imports std.math")
   assert(abs(-7) == 7, "lazy std import keeps the selected module active")
   print("REPL_LAZY_STD_" + "ROOT_OK")
   return 0
}
