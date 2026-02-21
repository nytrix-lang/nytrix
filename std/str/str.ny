;; Keywords: str slice
;; String helpers.

module std.str.str (
   str_slice, utf8_slice
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
         cnt += 1
         i = i + step
      }
   } else {
      while(i > stop){
         cnt += 1
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
         j += 1
         i = i + step
      }
   } else {
      while(i > stop){
         store8(out, load8(s, i), j)
         j += 1
         i = i + step
      }
   }
   store8(out, 0, cnt)
   out
}

fn utf8_slice(s, start, stop, step=1){
   "Returns a UTF-8 code-point slice of string `s`."
   if(!is_str(s)){ return 0 }
   if(!is_int(step)){ step = 1 }
   if(step == 0){ step = 1 }

   def n = utf8_len(s)
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

   mut out = ""
   mut i = start
   if(step > 0){
      while(i < stop){
         out = out + chr(ord_at(s, i))
         i = i + step
      }
   } else {
      while(i > stop){
         out = out + chr(ord_at(s, i))
         i = i + step
      }
   }
   out
}

if(comptime{__main()}){

    assert(str_slice("hello", 0, 5) == "hello", "str_slice full")
    assert(str_slice("hello", 1, 4) == "ell", "str_slice middle")
    assert(str_slice("hello", 0, 5, 2) == "hlo", "str_slice step")
    assert(str_slice("hello", -4, -1) == "ell", "str_slice negative bounds")
    assert(str_slice("hello", 4, 1, -1) == "oll", "str_slice reverse")
    assert(str_slice("hello", 0, 0) == "", "str_slice empty")
    assert(str_slice(1, 0, 1) == 0, "str_slice non-string")

    assert(utf8_slice("hello", 1, 4) == "ell", "utf8_slice middle")
    assert(utf8_slice("hello", 0, 5, 2) == "hlo", "utf8_slice step")
    assert(utf8_slice("hello", 4, 1, -1) == "oll", "utf8_slice reverse")
    assert(utf8_slice(1, 0, 1) == 0, "utf8_slice non-string")
}
