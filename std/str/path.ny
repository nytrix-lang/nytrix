;; Keywords: str path
;; Path helpers.

use std.core *
use std.core.reflect as core
use std.str *

module std.str.path (
   basename, dirname
)

fn dirname(path){
   "Returns the directory component of path."
   if(!is_str(path)){ return "." }
   def n = str_len(path)
   if(n == 0){ return "." }
   mut i = n - 1
   while(i >= 0){
      if(load8(path, i) == 47){ ; '/'
         if(i == 0){ return "/" }
         return core.slice(path, 0, i, 1)
      }
      i -= 1
   }
   return "."
}

fn basename(path){
   "Returns the last path component."
   if(!is_str(path)){ return "" }
   def n = str_len(path)
   if(n == 0){ return "" }
   mut i = n - 1
   while(i >= 0){
      if(load8(path, i) == 47){ ; '/'
         return core.slice(path, i + 1, n, 1)
      }
      i -= 1
   }
   return path
}
