;; Keywords: logic boolean
;; Math Logic for Nytrix
module std.math.logic(any, all)
use std.math
use std.core
use std.core.reflect

fn any(any: xs): bool {
   "Returns true if at least one element in `xs` is truthy. If `xs` is not a list, returns `bool(xs)`."
   if(!is_list(xs)){ return bool(xs) }
   mut i = 0
   while(i < xs.len){
      if(bool(xs.get(i))){ return true }
      i += 1
   }
   false
}

fn all(any: xs): bool {
   "Returns true if all elements in `xs` are truthy. If `xs` is not a list, returns `bool(xs)`."
   if(!is_list(xs)){ return bool(xs) }
   mut i = 0
   while(i < xs.len){
      if(!bool(xs.get(i))){ return false }
      i += 1
   }
   true
}
