;; Keywords: core dict
;; Core Dict module.

module std.core.dict (
   dict, dict_get, dict_set, dict_items, dict_keys, dict_values
)
use std.core *
use std.core.error *
use std.str *
use std.str.io *

fn _dict_pow2(n){
   "Internal: rounds `n` up to the next power-of-two."
   mut v = 1
   while(v < n){ v = v << 1 }
   v
}

fn _dict_str_eq(a, b){
   "Internal: byte-wise string equality for dictionary keys."
   if(!is_str(a) || !is_str(b)){ return false }
   def n = str_len(a)
   if(__eq(n, str_len(b)) == false){ return false }
   mut i = 0
   while(i < n){
      if(load8(a, i) != load8(b, i)){ return false }
      i = i + 1
   }
   return true
}

fn _dict_key_eq(a, b){
   "Internal: key equality with string fast-path."
   if(is_str(a) && is_str(b)){ return _dict_str_eq(a, b) }
   return __eq(a, b) || eq(a, b)
}

fn _dict_hash(x){
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

fn _dict_new(cap){
   "Internal: allocates a dictionary storage block."
   if(cap < 8){ cap = 8 }
   cap = _dict_pow2(cap)
   def p = malloc(16 + cap * 24)
   if(!p){ panic("dict malloc failed") }
   store64(p, 101, -8)
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

fn dict(cap=8){
   "Creates a new empty dictionary."
   _dict_new(cap)
}

fn _dict_insert(d, key, val){
   "Internal: inserts or replaces a key/value pair."
   def cap = load64(d, 8)
   def h = _dict_hash(key)
   def mask = cap - 1
   mut idx = h & mask
   mut perturb = h
   mut probes = 0
   while(probes < cap){
      def off = 16 + idx * 24
      def st = load64(d, off + 16)
      if(eq(st, 0)){
         store64(d, key, off)
         store64(d, val, off + 8)
         store64(d, 1, off + 16)
         store64(d, load64(d, 0) + 1, 0)
         return d
      }
      if(eq(st, 1)){
         if(_dict_key_eq(load64(d, off), key)){
            store64(d, val, off + 8)
            return d
         }
      }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes = probes + 1
   }
   return d
}

fn _dict_resize(d, newcap){
   "Internal: rebuilds the hash table at a larger capacity."
   mut nd = _dict_new(newcap)
   def cap = load64(d, 8)
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      def st = load64(d, off + 16)
      if(eq(st, 1)){
         nd = _dict_insert(nd, load64(d, off), load64(d, off + 8))
      }
      i = i + 1
   }
   free(d)
   nd
}

fn dict_set(d, key, val){
   "Sets key `key` to value `val` in dict `d`. Returns the (possibly reallocated) dict."
   if(!d){ d = _dict_new(8) }
   if(!is_dict(d)){ 
      print(f"DEBUG: dict_set failed on {d} tag {load64(d, -8)} is_ptr {is_ptr(d)}")
      panic("dict_set called on non-dictionary") 
   }
   def count = load64(d, 0)
   def cap = load64(d, 8)
   if(count * 10 >= cap * 7){
      d = _dict_resize(d, cap * 2)
   }
   _dict_insert(d, key, val)
}

fn dict_get(d, key, default=0){
   "Returns the value for `key` or `default` if missing."
   if(!is_dict(d)){ panic("dict_get called on non-dictionary") }
   def cap = load64(d, 8)
   def h = _dict_hash(key)
   def mask = cap - 1
   mut idx = h & mask
   mut perturb = h
   mut probes = 0
   while(probes < cap){
      def off = 16 + idx * 24
      def st = load64(d, off + 16)
      if(eq(st, 0)){ return default }
      if(eq(st, 1)){
         if(_dict_key_eq(load64(d, off), key)){
            return load64(d, off + 8)
         }
      }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes = probes + 1
   }
   return default
}

fn _dict_pair(a, b){
   "Internal: packs `(a, b)` into a two-element list."
   def p = list(2)
   store64(p, a, 16)
   store64(p, b, 24)
   store64(p, 2, 0)
   p
}

fn dict_items(d){
   "Returns a list of [key, value] pairs."
   if(!is_dict(d)){ return list(0) }
   def count = load64(d, 0)
   def out = list(count)
   def cap = load64(d, 8)
   mut i = 0
   mut idx = 0
   while(i < cap){
      def off = 16 + i * 24
      def st = load64(d, off + 16)
      if(eq(st, 1)){
         def pair = _dict_pair(load64(d, off), load64(d, off + 8))
         store64(out, pair, 16 + idx * 8)
         store64(out, idx + 1, 0)
         idx = idx + 1
      }
      i = i + 1
   }
   out
}

fn dict_keys(d){
   "Returns a list of keys."
   if(!is_dict(d)){ return list(0) }
   def count = load64(d, 0)
   def out = list(count)
   def cap = load64(d, 8)
   mut i = 0
   mut idx = 0
   while(i < cap){
      def off = 16 + i * 24
      def st = load64(d, off + 16)
      if(eq(st, 1)){
         store64(out, load64(d, off), 16 + idx * 8)
         store64(out, idx + 1, 0)
         idx = idx + 1
      }
      i = i + 1
   }
   out
}

fn dict_values(d){
   "Returns a list of values."
   if(!is_dict(d)){ return list(0) }
   def count = load64(d, 0)
   def out = list(count)
   def cap = load64(d, 8)
   mut i = 0
   mut idx = 0
   while(i < cap){
      def off = 16 + i * 24
      def st = load64(d, off + 16)
      if(eq(st, 1)){
         store64(out, load64(d, off + 8), 16 + idx * 8)
         store64(out, idx + 1, 0)
         idx = idx + 1
      }
      i = i + 1
   }
   out
}
