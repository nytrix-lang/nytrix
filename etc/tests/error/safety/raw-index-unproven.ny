;; flags: --safe-mode
;; expect: safe-mode raw memory access requires a proven byte range for index
use std.core

mut int: i = 0

if argc() > 0 { i = argc() }
with ptr: p = malloc(8){
   load8(p, i)
}
