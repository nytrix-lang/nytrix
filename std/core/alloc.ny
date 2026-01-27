;; Keywords: core alloc
;; Core Alloc module.

use std.core *
module std.core.alloc (
   bump_new, bump_alloc, bump_reset
)

fn bump_new(cap){
   "Create a new bump allocator with the specified capacity. Returns a state list [buffer, capacity, offset]."
   def buf = malloc(cap)
   return [buf, cap, 0]
}

fn bump_alloc(state, n){
   "Allocates `n` bytes from the bump allocator. Returns a pointer to the allocated memory, or 0 if the allocator is full."
   def base = state[0]  cap = state[1]  off = state[2]
   if(off + n > cap){ return 0  }
   def p = base + off
   set_idx(state, 2, off + n)
   return p
}

fn bump_reset(state){
   "Resets the bump allocator offset to 0, effectively freeing all allocated memory."
   set_idx(state, 2, 0)  return 0
}
