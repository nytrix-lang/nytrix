;; Keywords: tuple product-type
;; Core Tuple for Nytrix
module std.core.tuple(tuple)
use std.core
use std.core.primitives as prim

fn tuple(any: xs): tuple {
   "Creates a tuple from a list of elements. Tuples are immutable versions of lists."
   if(!is_list(xs)){ xs = [] }
   def out = _clone_list(xs)
   prim.list_as_tuple_raw(out)
}
