;; expect: cannot assign string literal to int
use std.core

fn bad(bool: flag): int {
   if(flag){ return 1 }
   return "x"
}

print(bad(false))
