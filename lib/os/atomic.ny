;; Keywords: atomic concurrency lock-free
;; First-class atomic 64-bit slots for shared counters and synchronization flags.
module std.os.atomic(atomic_i64, atomic_free, atomic_load, atomic_store, atomic_add, atomic_sub, atomic_exchange, atomic_compare_exchange)
use std.core

fn atomic_i64(int: initial=0): ptr {
   "Allocates an atomic Ny integer slot initialized to `initial`."
   def p = malloc(8)
   if(p){ __atomic_store64(p, 0, initial) }
   p
}

fn atomic_free(ptr: cell): any {
   "Frees an atomic slot allocated by `atomic_i64`."
   if(cell){ free(cell) }
   0
}

fn atomic_load(ptr: cell, int: offset=0): any {
   "Atomically loads the Ny value at `cell + offset`."
   __atomic_load64(cell, offset)
}

fn atomic_store(ptr: cell, any: value, int: offset=0): any {
   "Atomically stores `value` at `cell + offset` and returns `value`."
   __atomic_store64(cell, offset, value)
}

fn atomic_add(ptr: cell, any: delta=1, int: offset=0): any {
   "Atomically adds integer `delta` and returns the previous value."
   __atomic_add64(cell, offset, delta)
}

fn atomic_sub(ptr: cell, any: delta=1, int: offset=0): any {
   "Atomically subtracts integer `delta` and returns the previous value."
   __atomic_sub64(cell, offset, delta)
}

fn atomic_exchange(ptr: cell, any: value, int: offset=0): any {
   "Atomically replaces the value and returns the previous value."
   __atomic_exchange64(cell, offset, value)
}

fn atomic_compare_exchange(ptr: cell, any: expected, any: desired, int: offset=0): bool {
   "Atomically swaps `expected` to `desired`; returns true on success."
   __atomic_cas64(cell, offset, expected, desired)
}
