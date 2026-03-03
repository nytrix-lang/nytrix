use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *

;; If strict syntax (Test)

fn early_return(x){
   if(x < 0){ return 0 }
   if(x == 10){
      return 20
   } else {
      return x * 2
   }
}

assert(early_return(-1) == 0, "if return")
assert(early_return(10) == 20, "if else return")
assert(early_return(5) == 10, "fallthrough")

if(1){ print("if true ok") }

if(0){
   panic("if false taken")
} else {
   print("if false else ok")
}

mut val = 0
if(1){
   val = 1
   val = 2
}
assert(val == 2, "block exec")

if(0){
   val = 10
}
val = 20
assert(val == 20, "skip block")

print("✓ if strict syntax tests passed")
