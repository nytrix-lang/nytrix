;; Keywords: core iter
;; Iter helpers.

use std.core *
module std.core.iter (
   range
)

fn range(stop, start=0, step=1){
   "Returns a list of integers from start to stop (exclusive)."
   mut s = 0
   mut e = stop
   mut st = step
   if(start != 0){
      s = stop
      e = start
      st = step
   }
   if(st == 0){ st = 1 }
   mut out = list(8)
   if(st > 0){
      mut i = s
      while(i < e){
         out = append(out, i)
         i += st
      }
   } else {
      mut i = s
      while(i > e){
         out = append(out, i)
         i += st
      }
   }
   out
}
