;; Keywords: str slice
;; String helpers.

module std.str.str (
   str_slice
)
use std.core *
use std.str *

fn str_slice(s, start, stop, step=1){
   "Returns a slice of string `s` from `start` to `stop` with optional `step`."
   if(!is_str(s)){ return 0 }
   def n = str_len(s)
   if(step == 0){ step = 1 }
   if(start < 0){ start = n + start }
   if(stop < 0){ stop = n + stop }
   if(step > 0){
      if(start < 0){ start = 0 }
      if(stop > n){ stop = n }
      if(start >= stop){ return "" }
   } else {
      if(start >= n){ start = n - 1 }
      if(stop < -1){ stop = -1 }
      if(start <= stop){ return "" }
   }
   mut cnt = 0
   mut i = start
   if(step > 0){
      while(i < stop){
         cnt = cnt + 1
         i = i + step
      }
   } else {
      while(i > stop){
         cnt = cnt + 1
         i = i + step
      }
   }
   def out = malloc(cnt + 1)
   if(!out){ return 0 }
   init_str(out, cnt)
   i = start
   mut j = 0
   if(step > 0){
      while(i < stop){
         store8(out, load8(s, i), j)
         j = j + 1
         i = i + step
      }
   } else {
      while(i > stop){
         store8(out, load8(s, i), j)
         j = j + 1
         i = i + step
      }
   }
   store8(out, 0, cnt)
   out
}
