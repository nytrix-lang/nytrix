;; flags: --safe-mode
use std.core

with ptr p = malloc(8){
   store8(p, 65, 0)
   assert(load8(p, 0) == 65, "safe-mode literal raw load")
   def int idx = 3
   assert_compile_range(idx, 0, 7, "safe-mode raw byte index proof")
   store8(p, 66, idx)
   assert(load8(p, idx) == 66, "safe-mode proven raw load")
}

print("✓ safe mode test passed")
