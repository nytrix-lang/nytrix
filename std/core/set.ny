;; Keywords: core set
;; Core Set module.

use std.core *
use std.core.error *
use std.str *
use std.str.io *

module std.core.set (
   set, set_add, set_contains
)

fn _set_pow2(n){
   "Internal: rounds `n` up to the next power-of-two."
   mut v = 1
   while(v < n){ v = v << 1 }
   v
}

fn _set_str_eq(a, b){
   "Internal: byte-wise string equality for set keys."
   if(!is_str(a) || !is_str(b)){ return false }
   def n = str_len(a)
   if(eq(n, str_len(b)) == false){ return false }
   mut i = 0
   while(i < n){
      if(load8(a, i) != load8(b, i)){ return false }
      i = i + 1
   }
   return true
}

fn _set_key_eq(a, b){
   "Internal: key equality with string fast-path."
   if(is_str(a) && is_str(b)){ return _set_str_eq(a, b) }
   return eq(a, b)
}

fn _set_hash(x){
   "Internal: hashes integers/strings for open-addressing lookup."
   if(is_int(x)){ return x }
   if(is_str(x)){
      mut h = 14695981039346656037
      mut i = 0
      def n = str_len(x)
      while(i < n){
         h = (h ^ load8(x, i)) * 1099511628211
         i = i + 1
      }
      return h
   }
   return x
}

fn _set_new(cap){
   "Internal: allocates a set storage block."
   if(cap < 8){ cap = 8 }
   cap = _set_pow2(cap)
   def p = malloc(16 + cap * 24)
   if(!p){ panic("set malloc failed") }
   store64(p, 102, -8)
   store64(p, 0, 0)
   store64(p, cap, 8)
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      store64(p, 0, off)
      store64(p, 0, off + 8)
      store64(p, 0, off + 16)
      i = i + 1
   }
   p
}

fn set(cap=8){
   "Creates a new empty set."
   _set_new(cap)
}

fn _set_insert(s, key){
   "Internal: inserts a key if it is not already present."
   def cap = load64(s, 8)
   def h = _set_hash(key)
   def mask = cap - 1
   mut idx = h & mask
   mut perturb = h
   mut probes = 0
   while(probes < cap){
      def off = 16 + idx * 24
      def st = load64(s, off + 16)
      if(eq(st, 0)){
         store64(s, key, off)
         store64(s, 1, off + 8)
         store64(s, 1, off + 16)
         store64(s, load64(s, 0) + 1, 0)
         return s
      }
      if(eq(st, 1)){
         if(_set_key_eq(load64(s, off), key)){
            return s
         }
      }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes = probes + 1
   }
   return s
}

fn _set_resize(s, newcap){
   "Internal: rebuilds the set at a larger capacity."
   mut ns = _set_new(newcap)
   def cap = load64(s, 8)
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      def st = load64(s, off + 16)
      if(eq(st, 1)){
         ns = _set_insert(ns, load64(s, off))
      }
      i = i + 1
   }
   free(s)
   ns
}

fn set_add(s, key){
   "Adds `key` to set `s`. Returns the (possibly reallocated) set."
   if(!s){ s = _set_new(8) }
   if(!is_set(s)){ 
      print(f"DEBUG: set_add failed on {s} tag {load64(s, -8)}")
      panic("set_add called on non-set") 
   }
   def count = load64(s, 0)
   def cap = load64(s, 8)
   if(count * 10 >= cap * 7){
      s = _set_resize(s, cap * 2)
   }
   _set_insert(s, key)
}

fn set_contains(s, key){
   "Returns true if `key` is in set `s`."
   if(!is_set(s)){ return false }
   def cap = load64(s, 8)
   def h = _set_hash(key)
   def mask = cap - 1
   mut idx = h & mask
   mut perturb = h
   mut probes = 0
   while(probes < cap){
      def off = 16 + idx * 24
      def st = load64(s, off + 16)
      if(eq(st, 0)){ return false }
      if(eq(st, 1)){
         if(_set_key_eq(load64(s, off), key)){
            return true
         }
      }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes = probes + 1
   }
   return false
}
