;; Keywords: set hashset core
;; Set construction, membership, mutation, and set algebra operations.
;; References:
;; - std.core
module std.core.set_mod(set, add, sub, contains, len, clear, values, is_empty, _set_add, _set_remove, _set_contains, _set_len, _set_values, _set_clear)
use std.core
use std.core.primitives as prim

@inline
fn _set_str_eq(any a, any b) bool {
   if !is_str(a) || !is_str(b) { return false }
   def n = a.len
   if n != b.len { return false }
   memcmp(a, b, n) == 0
}

@inline
fn _set_key_eq(any a, any b) bool {
   if is_str(a) && is_str(b) { return _set_str_eq(a, b) }
   a == b
}

@inline
fn _set_hash(any x) int {
   if is_int(x) { return x }
   if is_str(x) {
      mut h, i = 2166136261, 0
      def n = x.len
      while i < n {
         h = ((h ^^ load8(x, i)) * 16777619) & 2147483647
         i += 1
      }
      return h
   }
   return x
}

@inline
fn _set_find_existing_off(set s, any key) int {
   def cap = load64(s, 8)
   mut i = 0
   while i < cap {
      def off = 16 + i * 24
      if load64(s, off + 16) == 1 && _set_key_eq(load64(s, off), key) { return off }
      i += 1
   }
   -1
}

@inline
@returns_owned
@consumes(s)
fn _set_tombstone_at(set s, int off) set {
   store64(s, 0, off)
   store64(s, 0, off + 8)
   store64(s, 2, off + 16)
   store64(s, load64(s, 0) - 1, 0)
   s
}

@returns_owned
fn _set_new(int cap) set {
   if cap < 8 { cap = 8 }
   cap = _pow2(cap)
   def p = __malloc(16 + cap * 24)
   if !p { panic("set malloc failed") }
   store64(p, prim.runtime_tag_raw("set"), -8)
   store64(p, 0, 0)
   store64(p, cap, 8)
   mut i = 0
   while i < cap {
      def off = 16 + i * 24
      store64(p, 0, off)
      store64(p, 0, off + 8)
      store64(p, 0, off + 16)
      i += 1
   }
   p
}

@inline
@returns_owned
fn set(int cap=8) set {
   "Creates a new empty set."
   _set_new(cap)
}

@inline
@returns_owned
@consumes(s)
fn _set_insert(set s, any key) set {
   def cap = load64(s, 8)
   if is_str(key) && _set_find_existing_off(s, key) >= 0 { return s }
   def h = _set_hash(key)
   def mask = cap - 1
   mut idx = h & mask
   mut perturb = h
   mut probes = 0
   while probes < cap {
      def off = 16 + idx * 24
      def st = load64(s, off + 16)
      case st {
         0 -> {
            store64(s, retain(key), off)
            store64(s, 1, off + 8)
            store64(s, 1, off + 16)
            store64(s, load64(s, 0) + 1, 0)
            return s
         }
         1 if _set_key_eq(load64(s, off), key) -> { return s }
         _ -> {}
      }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes += 1
   }
   return s
}

@returns_owned
@consumes(s)
fn _set_resize(set s, int newcap) set {
   mut ns = _set_new(newcap)
   def cap = load64(s, 8)
   mut i = 0
   while i < cap {
      def off = 16 + i * 24
      def st = load64(s, off + 16)
      if st == 1 { ns = _set_insert(ns, load64(s, off)) }
      i += 1
   }
   free(s)
   ns
}

@inline
fn _set_has_existing(any s, any key) bool {
   if !is_set(s) { return false }
   if is_str(key) { return _set_find_existing_off(s, key) >= 0 }
   def cap = load64(s, 8)
   def h = _set_hash(key)
   def mask = cap - 1
   mut idx = h & mask
   mut perturb = h
   mut probes = 0
   while probes < cap {
      def off = 16 + idx * 24
      def st = load64(s, off + 16)
      case st {
         0 -> { return false }
         1 if _set_key_eq(load64(s, off), key) -> { return true }
         _ -> {}
      }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes += 1
   }
   return false
}

@returns_owned
fn _set_add(any s, any key) set {
   if !s { s = _set_new(8) }
   if !is_set(s) { panic("add on non-set") }
   if _set_has_existing(s, key) { return s }
   def count = load64(s, 0)
   def cap = load64(s, 8)
   if count * 10 >= cap * 7 {
      def ns = _set_resize(s, cap * 2)
      return _set_insert(ns, key)
   }
   return _set_insert(s, key)
}

@inline
fn _set_contains(any s, any key) bool { _set_has_existing(s, key) }

@returns_owned
fn _set_remove(any s, any key) any {
   if !is_set(s) { return s }
   if is_str(key) {
      def off = _set_find_existing_off(s, key)
      if off >= 0 { return _set_tombstone_at(s, off) }
      return s
   }
   def cap = load64(s, 8)
   def h = _set_hash(key)
   def mask = cap - 1
   mut idx = h & mask
   mut perturb = h
   mut probes = 0
   while probes < cap {
      def off = 16 + idx * 24
      def st = load64(s, off + 16)
      case st {
         0 -> { return s }
         1 if _set_key_eq(load64(s, off), key) -> {
            return _set_tombstone_at(s, off)
         }
         _ -> {}
      }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes += 1
   }
   s
}

@inline
fn _set_len(any s) int {
   if !is_set(s) { return 0 }
   load64(s, 0)
}

@inline
fn len(any s) int {
   "Returns the number of elements in set `s`."
   _set_len(s)
}

@inline
fn is_empty(any s) bool {
   "Returns true if set `s` has no elements."
   _set_len(s) == 0
}

@returns_owned
fn _set_values(any s) list {
   if !is_set(s) { return list(0) }
   def cap = load64(s, 8)
   def count = load64(s, 0)
   mut out = list(count)
   mut idx = 0
   mut i = 0
   while i < cap {
      def off = 16 + i * 24
      def st = load64(s, off + 16)
      if st == 1 {
         store64(out, load64(s, off), 16 + idx * 8)
         idx += 1
      }
      i += 1
   }
   store64(out, idx, 0)
   out
}

@returns_owned
fn _set_clear(any s) any {
   if !is_set(s) { return s }
   def cap = load64(s, 8)
   mut i = 0
   while i < cap {
      def off = 16 + i * 24
      store64(s, 0, off)
      store64(s, 0, off + 8)
      store64(s, 0, off + 16)
      i += 1
   }
   store64(s, 0, 0)
   s
}

@inline
@returns_owned
fn add(any s, any key) set {
   "Adds `key` to set `s`."
   _set_add(s, key)
}

@inline
@returns_owned
fn sub(any s, any key) any {
   "Removes `key` from set `s`."
   _set_remove(s, key)
}

@inline
fn contains(any s, any key) bool {
   "Returns true if `key` is in set `s`."
   _set_contains(s, key)
}

@inline
@returns_owned
fn clear(any s) any {
   "Removes all elements from set `s`."
   _set_clear(s)
}

@inline
@returns_owned
fn values(any s) list {
   "Returns the values of set `s` as a list."
   _set_values(s)
}
