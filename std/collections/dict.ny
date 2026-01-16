;; Keywords: collections dict
;; Collections Dict module.

use std.core
use std.core.reflect
module std.collections.dict (
   dict, dict_resize, dict_set, dict_get, dict_contains, dict_del, dict_items, dict_keys, dict_values, dict_clear,
   dict_copy, dict_update
)

def ENTRY_SIZE     = 24
def HEADER_SIZE    = 16

fn dict(cap=8){
   "Creates a new empty **dictionary** with the specified initial capacity `cap` (default 8). Uses an open-addressed hash table with quadratic probing."
   if(cap < 8){ cap = 8 }
   def c = 8
   while(c < cap){ c = c * 2 }
   def d = __malloc(16 + c * 24)
   store64(d, 101, -8)
   store64(d, 0, 0)
   store64(d, c, 8)
   def i = 0
   while(i < c){ store64(d, 0, 16 + i * 24 + 16)  i += 1 }
   d
}

fn dict_resize(d, new_cap){
   "Internal: Resizes the hash table of dictionary `d` to `new_cap` and rehashes all existing entries."
   def old_cap = load64(d, 8)
   def new_d = dict(new_cap)
   def i = 0
   while(i < old_cap){
      def off = 16 + i * 24
      if(load64(d, off + 16) == 1){
         dict_set(new_d, load64(d, off), load64(d, off + 8))
      }
      i += 1
   }
   new_d
}

fn dict_set(d, key, val){
   "Inserts or updates the value `val` for the given `key` in dictionary `d`."
   def count = load64(d, 0)
   def cap = load64(d, 8)
   if(count * 2 >= cap){
      d = dict_resize(d, cap * 2)
      count = load64(d, 0)
      cap = load64(d, 8)
   }
   def h = hash(key)  def mask = cap - 1  def idx = h & mask  def perturb = h  def probes = 0
   def first_free = -1
   while(probes < cap){
      def off = 16 + idx * 24
      def st = load64(d, off + 16)
      if(st == 0){
         if(first_free != -1){ off = first_free }
         store64(d, key, off)
         store64(d, val, off + 8)
         store64(d, 1, off + 16)
         store64(d, count + 1, 0)
         return d
      }
      if(st == 2){ if(first_free == -1){ first_free = off } }
      if(st == 1){
         if(eq(load64(d, off), key)){ store64(d, val, off + 8)  return d }
      }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes += 1
   }
   if(first_free != -1){
       def off = first_free
       store64(d, key, off)
       store64(d, val, off + 8)
       store64(d, 1, off + 16)
       store64(d, count + 1, 0)
   }
   d
}

fn dict_get(d, key, default_val=0){
   "Retrieves the value associated with `key` in dictionary `d`. Returns `default_val` (default 0) if the key is not found."
   def cap = load64(d, 8)  def h = hash(key)  def mask = cap - 1  def idx = h & mask  def perturb = h  def probes = 0
   while(probes < cap){
      def off = 16 + idx * 24
      def st = load64(d, off + 16)
      if(st == 0){ return default_val }
      if(st == 1){
         if(eq(load64(d, off), key)){ return load64(d, off + 8) }
      }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes += 1
   }
   default_val
}

fn dict_contains(d, key){
   "Returns **true** if the `key` exists within dictionary `d`."
   def cap = load64(d, 8)  def h = hash(key)  def mask = cap - 1  def idx = h & mask  def perturb = h  def probes = 0
   while(probes < cap){
      def off = 16 + idx * 24
      def st = load64(d, off + 16)
      if(st == 0){ return false }
      if(st == 1){ if(eq(load64(d, off), key)){ return true } }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes += 1
   }
   false
}

fn dict_del(d, key){
   "Removes `key` and its associated value from dictionary `d`. Returns the updated dictionary."
   def cap = load64(d, 8)  def h = hash(key)  def mask = cap - 1  def idx = h & mask  def perturb = h  def probes = 0
   while(probes < cap){
      def off = 16 + idx * 24
      def st = load64(d, off + 16)
      if(st == 0){ return d }
      if(st == 1){
         if(eq(load64(d, off), key)){
            store64(d, 2, off + 16)
            store64(d, load64(d, 0) - 1, 0)
            return d
         }
      }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes += 1
   }
   d
}

fn dict_items(d){
   "Returns a [[std.core::list]] of all `[key, value]` pairs in dictionary `d`."
   def res = list(8)  def cap = load64(d, 8)  def i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(load64(d, off + 16) == 1){ res = append(res, [load64(d, off), load64(d, off + 8)]) }
      i += 1
   }
   res
}

fn dict_keys(d){
   "Returns a [[std.core::list]] containing all keys present in dictionary `d`."
   def res = list(8)  def cap = load64(d, 8)  def i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(load64(d, off + 16) == 1){ res = append(res, load64(d, off)) }
      i += 1
   }
   res
}

fn dict_values(d){
   "Returns a [[std.core::list]] containing all values present in dictionary `d`."
   def res = list(8)  def cap = load64(d, 8)  def i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(load64(d, off + 16) == 1){ res = append(res, load64(d, off + 8)) }
      i += 1
   }
   res
}

fn dict_clear(d){
   "Removes all items from dictionary `d`, resetting its state."
   def cap = load64(d, 8)
   def i = 0
   while(i < cap){
      store64(d, 0, 16 + i * 24 + 16)
      i += 1
   }
   store64(d, 0, 0)
   d
}

fn dict_copy(d){
   "Returns a **shallow copy** of dictionary `d`."
   def cap = load64(d, 8)
   def out = dict(cap)
   def i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(load64(d, off + 16) == 1){
         dict_set(out, load64(d, off), load64(d, off + 8))
      }
      i += 1
   }
   out
}

fn dict_update(d, other){
   "Updates dictionary `d` with items from another mapping or iterable `other`."
   if(is_dict(other)){
      def its = dict_items(other)  def i = 0  def n = list_len(its)
      while(i < n){ def p = get(its, i)  dict_set(d, p[0], p[1])  i += 1 }
   } else {
      def i = 0  def n = list_len(other)
      while(i < n){ def p = get(other, i)  dict_set(d, p[0], p[1])  i += 1 }
   }
   d
}