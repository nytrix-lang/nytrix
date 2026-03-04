;; Keywords: core dict
;; Hash Dictionary Implementation for Nytrix

module std.core.dict_mod (
   dict, dict_len, dict_get, dict_has, dict_set, dict_del, dict_clone, dict_merge,
   dict_items, dict_keys, dict_values
)
use std.core *
use std.core.error *
use std.str *
use std.str.io *

@inline fn _dict_str_eq(a, b){
   "Internal: byte-wise string equality for dictionary keys using memcmp."
   if(!is_str(a) || !is_str(b)){ return false }
   def n = str_len(a)
   if(n != str_len(b)){ return false }
   return memcmp(a, b, n) == 0
}

@inline fn _dict_key_eq(a, b){
   "Internal: key equality with string fast-path."
   if(is_str(a) && is_str(b)){
      return _dict_str_eq(a, b)
   }
   return (a == b)
}

fn _dict_hash(x){
   "Computes a 31-bit tagged hash for `x`."
   if(!x){ return 0 }
   if(is_int(x)){ return x }
   if(is_str(x)){
      mut h = 2166136261
      def n = str_len(x)
      mut i = 0
      while(i < n){
         h = bxor(h, load8(x, i)) * 16777619
         i = i + 1
      }
      return band(h, 2147483647)
   }
   if(is_ptr(x)){
      return band(bshr(to_int(x), 3), 2147483647)
   }
   return 0
}

fn _dict_new(cap){
   "Internal: allocates a new dictionary with the given capacity."
   ; Header: count(8) + cap(8) = 16 bytes
   ; Each slot: key(8) + val(8) + state(8, 0=empty, 1=filled) = 24 bytes
   def size = 16 + cap * 24
   mut p = __malloc(size)
   memset(p, 0, size)
   store8(p, 101, -8) ; tag
   store64(p, 0, 0) ; count (tagged 0)
   store64(p, cap, 8) ; cap
   p
}

fn dict(cap=8){
   "Creates a new empty dictionary."
   _dict_new(_pow2(cap))
}

@inline fn dict_len(d){
   "Returns the number of entries in dictionary `d`."
   if(!is_dict(d)){ return 0 }
   load64(d, 0)
}

fn _dict_find_off(d, key){
   "Internal: finds the offset for a key, or returns where it should be inserted."
   def cap = load64(d, 8)
   def mask = cap - 1
   def h = _dict_hash(key)
   mut idx = h & mask
   mut perturb = h
   mut i = 0
   while(i < (cap + 32)){
      def off = 16 + idx * 24
      def state = load64(d, off + 16)
      if(!state){ return off }
      if(_dict_key_eq(load64(d, off), key)){ return off }

      perturb = perturb >> 5
      idx = (idx * 5 + perturb + 1) & mask
      i = i + 1
   }
   return -1
}

fn _dict_resize(d){
   "Internal: grows the dictionary when it becomes too full."
   def old_cap = load64(d, 8)
   def new_cap = old_cap * 2
   mut nd = _dict_new(new_cap)

   mut i = 0
   while(i < old_cap){
      def off = 16 + i * 24
      if(load64(d, off + 16)){
         nd = dict_set(nd, load64(d, off), load64(d, off + 8))
      }
      i = i + 1
   }
   nd
}

fn dict_set(d, key, val){
   "Inserts or updates a key/value pair in dictionary `d`."
   if(!is_dict(d)){ return d }

   def tc = load64(d, 0)
   def tca = load64(d, 8)
   if(tc * 3 > tca * 2){
      mut nd = _dict_resize(d)
      return dict_set(nd, key, val)
   }

   def off = _dict_find_off(d, key)
   if(off < 0){ panic("Dictionary overflow") }

   def state = load64(d, off + 16)
   if(!state){
      store64(d, key, off)
      store64(d, val, off + 8)
      store64(d, 1, off + 16)
      store64(d, tc + 1, 0) ; Increment tagged count
   } else {
      store64(d, val, off + 8)
   }
   d
}

@inline fn dict_get(d, key, default=0){
   "Retrieves the value for `key` in `d`, or returns `default` if not found."
   if(!is_dict(d)){ return default }
   def off = _dict_find_off(d, key)
   if(off < 0 || !load64(d, off + 16)){ return default }
   load64(d, off + 8)
}

@inline fn dict_has(d, key){
   "Returns **true** if `key` exists in dictionary `d`."
   if(!is_dict(d)){ return false }
   def off = _dict_find_off(d, key)
   if(off < 0 || !load64(d, off + 16)){ return false }
   true
}

@inline fn dict_del(d, key){
   "Removes `key` from dictionary `d`. Returns the dictionary."
   if(!is_dict(d)){ return d }
   def off = _dict_find_off(d, key)
   if(off >= 0 && load64(d, off + 16)){
      store64(d, 0, off + 16)
      store64(d, load64(d, 0) - 1, 0) ; Decrement tagged count
   }
   d
}

fn dict_clone(d){
   "Creates a shallow copy of dictionary `d`."
   if(!is_dict(d)){ return d }
   def cap = load64(d, 8)
   mut nd = _dict_new(cap)
   store64(nd, load64(d, 0), 0)

   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(load64(d, off + 16)){
         store64(nd, load64(d, off), off)
         store64(nd, load64(d, off + 8), off + 8)
         store64(nd, 1, off + 16)
      }
      i = i + 1
   }
   nd
}

fn dict_merge(dst, src){
   "Merges `src` into `dst` (overwriting duplicate keys). Returns merged dictionary."
   if(!is_dict(dst) || !is_dict(src)){ return dst }
   def cap = load64(src, 8)
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(load64(src, off + 16)){
         dst = dict_set(dst, load64(src, off), load64(src, off + 8))
      }
      i = i + 1
   }
   dst
}

@inline fn _dict_pair(a, b){
   "Internal: packs `(a, b)` into a two-element list."
   mut p = list(2)
   append(p, a)
   append(p, b)
   p
}

fn dict_items(d){
   "Returns a list of [key, value] pairs."
   if(!is_dict(d)){ return list() }
   mut out = list()
   def cap = load64(d, 8)
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(load64(d, off + 16)){
         out = append(out, _dict_pair(load64(d, off), load64(d, off + 8)))
      }
      i = i + 1
   }
   out
}

fn dict_keys(d){
   "Returns a list of keys."
   if(!is_dict(d)){ return list() }
   mut out = list()
   def cap = load64(d, 8)
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(load64(d, off + 16)){
         out = append(out, load64(d, off))
      }
      i = i + 1
   }
   out
}

fn dict_values(d){
   "Returns a list of values."
   if(!is_dict(d)){ return list() }
   mut out = list()
   def cap = load64(d, 8)
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(load64(d, off + 16)){
         out = append(out, load64(d, off + 8))
      }
      i = i + 1
   }
   out
}

if(comptime{__main()}){
   use std.core *
   use std.core.dict_mod *
   use std.core.test *

   mut d = dict(4)
   assert(dict_len(d) == 0, "dict_len empty")
   assert(!dict_has(d, "a"), "dict_has missing")

   d = dict_set(d, "a", 1)
   d = dict_set(d, "b", 2)
   d = dict_set(d, "c", 3)
   assert(dict_len(d) == 3, "dict_len after set")
   assert(dict_has(d, "b"), "dict_has present")
   assert(dict_get(d, "b", 0) == 2, "dict_get present")
   assert(dict_get(d, "x", 77) == 77, "dict_get default")

   mut d2 = dict_clone(d)
   d2 = dict_set(d2, "b", 22)
   assert(dict_get(d, "b", 0) == 2, "dict_clone original unchanged")
   assert(dict_get(d2, "b", 0) == 22, "dict_clone modified copy")

   mut d3 = dict(4)
   d3 = dict_set(d3, "b", 200)
   d3 = dict_set(d3, "d", 4)
   d = dict_merge(d, d3)
   assert(dict_len(d) == 4, "dict_merge len")
   assert(dict_get(d, "b", 0) == 200, "dict_merge overwrite")
   assert(dict_get(d, "d", 0) == 4, "dict_merge new key")

   d = dict_del(d, "b")
   assert(!dict_has(d, "b"), "dict_del removed")
   assert(dict_len(d) == 3, "dict_del len")

   def ks = dict_keys(d)
   def vs = dict_values(d)
   def it = dict_items(d)
   assert(len(ks) == 3, "dict_keys len")
   assert(len(vs) == 3, "dict_values len")
   assert(len(it) == 3, "dict_items len")

   print("✓ std.core.dict tests passed")
}
