;; Keywords: os args
;; Args helpers.

module std.os.args (
   args, argv
)
use std.core *

fn argv(i){
   "Returns the argv string at index `i`, or 0."
   return __argv(i)
}

fn args(){
   "Returns a list of argv strings."
   def n = __argc()
   mut out = list(8)
   mut i = 0
   while(i < n){
      out = append(out, __argv(i))
      i += 1
   }
   out
}
