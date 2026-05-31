;; repl-expect: REPL_COMPTIME_IMPORTS_OK
use std

def base = comptime{ 2^5 }
def shifted = comptime{ range(4).map(fn(i){ i + base }) }

fn main(): int {
   assert_eq(base, 32, "REPL comptime integer constant")
   assert_eq(to_str(shifted), "[32, 33, 34, 35]", "REPL comptime std imports")
   print("REPL_COMPTIME_" + "IMPORTS_OK")
   return 0
}
