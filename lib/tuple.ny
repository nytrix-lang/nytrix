;; Keywords: tuple product-type core
;; Core Tuple for Nytrix
;; References:
;; - core
module core.tuple(tuple)
use core
use core.primitives as core_prim

fn tuple(any xs) tuple {
   "Creates a tuple from a list of elements. Tuples are immutable versions of lists."
   if !is_list(xs) { xs = [] }
   def out = _clone_list(xs)
   core_prim.list_as_tuple_raw(out)
}

#main {
   def t = tuple([1, 2, 3])
   assert(is_tuple(t), "tuple type")
   assert(t.len == 3, "tuple len")
   assert(t.get(0) == 1 && t.get(2) == 3, "tuple get")
   assert(!is_list(t), "tuple not list")
   assert(type(t) == "tuple", "tuple reflected type")
   def empty = tuple(0)
   assert(is_tuple(empty), "tuple from non-list")
   assert(empty.len == 0, "empty tuple len")
   print("✓ core.tuple self-test passed")
}
