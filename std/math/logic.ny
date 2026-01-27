;; Keywords: math logic
;; Math Logic module.

use std.math *
use std.core *
use std.core.reflect *
module std.math.logic (
   any, all
)

fn any(xs){
   "Any true?"
   if(is_list(xs)==0){ return bool(xs)  }
   mut i =0
   while(i<list_len(xs)){
      if(bool(get(xs,i))==1){ return 1  }
      i=i+1
   }
   return 0
}

fn all(xs){
   "All true?"
   if(is_list(xs)==0){ return bool(xs)  }
   mut i =0
   while(i<list_len(xs)){
      if(bool(get(xs,i))==0){ return 0  }
      i=i+1
   }
   return 1
}

