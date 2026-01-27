;; Keywords: core tuple
;; Core Tuple module.

use std.core *
module std.core.tuple (
   tuple
)

fn tuple(xs){
   "Create a new tuple from a list of elements. Tuples are immutable versions of lists."
   if(is_list(xs) == false){ return tuple([]) }
   def out = list_clone(xs)
   store64(out, 103, -8) ; Set TUPLE tag (103) at -8
   return out
}
