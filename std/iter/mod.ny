;; Keywords: iter mod
;; Iter Mod module.

use std.core
module std.iter (
   range, map, filter, reduce
)

fn range(start, stop="<undef>", step=1){
   "Returns a list of integers from start to stop (exclusive) with step."
   if(step==0){ step=1 }
   if(is_str(stop) || stop == 0){
      if(stop == 0){
          ; Assume range(n) if step > 0
          if(step > 0){
             stop = start
             start = 0
          }
      } else {
          ; String sentinel (<undef>)
          stop = start
          start = 0
      }
   }
   def out = list(8)
   def i = start
   if(step > 0){
      while(i < stop){
         out = append(out, i)
         i = i + step
      }
   } else {
       while(i > stop){
         out = append(out, i)
         i = i + step
      }
   }
   return out
}

fn map(iter, f){
   "Applies function f to each element of iter."
   if(is_list(iter)){
      def out = list(list_len(iter))
      def out = list(list_len(iter))
      def i=0
      def n=list_len(iter)
      while(i<n){
         out = append(out, f(get(iter, i)))
         i=i+1
      }
      return out
   }
   return 0
}

fn filter(iter, f){
   "Returns a new list with elements where f(x) is true."
   if(is_list(iter)){
      def out = list(8)
      def out = list(8)
      def i=0
      def n=list_len(iter)
      while(i<n){
         def v = get(iter, i)
         if(f(v)){ out = append(out, v) }
         i=i+1
      }
      return out
   }
   return 0
}

fn reduce(iter, f, init){
   "Reduces iter using binary function f and initial value."
    if(is_list(iter)){
      def acc = init
      def acc = init
      def i=0
      def n=list_len(iter)
      while(i<n){
         acc = f(acc, get(iter, i))
         i=i+1
      }
      return acc
   }
   return init
}