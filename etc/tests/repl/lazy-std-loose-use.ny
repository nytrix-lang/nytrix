;; repl-expect: REPL_LAZY_STD_LOOSE_USE_OK
use std math

fn main(): int {
   assert(abs(10) == 10, "REPL normalizes loose std module imports")
   print("REPL_LAZY_STD_" + "LOOSE_USE_OK")
   return 0
}
