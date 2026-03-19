;; flags: --safe-mode
;; expect: safe-mode raw memory access out of bounds
use std.core

with ptr: p = malloc(4){
   load32(p, 2)
}
