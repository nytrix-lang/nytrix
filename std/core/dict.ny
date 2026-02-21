;; Keywords: core dict
;; Core Dict module.

module std.core.dict (
   dict, dict_len, dict_get, dict_has, dict_set, dict_del, dict_clone, dict_merge,
   dict_items, dict_keys, dict_values
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
   if(!(n == str_len(b))){ return false }
   mut i = 0
   while(i < n){
      if(load8(a, i) != load8(b, i)){ return false }
      i += 1
   }
   return true
}

fn _dict_key_eq(a, b){
   "Internal: key equality with string fast-path."
   if(is_str(a) && is_str(b)){ return _dict_str_eq(a, b) }
   return (a == b)
}

fn _dict_hash(x){
   "Internal: hashes integers/strings for open-addressing lookup."
   if(is_int(x)){ return x }
   if(is_str(x)){
      ;; Keep hash math in a 31-bit lane to avoid large-int edge cases on
      ;; narrower architectures.
      mut h = 2166136261
      mut i = 0
      def n = str_len(x)
      while(i < n){
         h = (h ^ load8(x, i)) * 16777619
         h = h & 2147483647
         i += 1
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
      i += 1
   }
   p
}

fn dict(cap=8){
   "Creates a new empty dictionary."
   _dict_new(cap)
}

fn dict_len(d){
   "Returns the number of entries in dictionary `d`."
   if(!is_dict(d)){ return 0 }
   load64(d, 0)
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
      if(st == 0){
         store64(d, key, off)
         store64(d, val, off + 8)
         store64(d, 1, off + 16)
         store64(d, load64(d, 0) + 1, 0)
         return d
      }
      if(st == 1){
         if(_dict_key_eq(load64(d, off), key)){
            store64(d, val, off + 8)
            return d
         }
      }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes += 1
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
      if(st == 1){
         nd = _dict_insert(nd, load64(d, off), load64(d, off + 8))
      }
      i += 1
   }
   free(d)
   nd
}

fn dict_set(d, key, val){
   "Sets key `key` to value `val` in dict `d`. Returns the (possibly reallocated) dict."
   if(!d){ d = _dict_new(8) }
   if(!is_dict(d)){ panic("dict_set called on non-dictionary") }
   def count = load64(d, 0)
   def cap = load64(d, 8)
   if(count * 10 >= cap * 7){
      d = _dict_resize(d, cap * 2)
   }
   _dict_insert(d, key, val)
}

fn _dict_find_off(d, key){
   "Internal: returns slot offset for `key`, or -1 when not found."
   def cap = load64(d, 8)
   def h = _dict_hash(key)
   def mask = cap - 1
   mut idx = h & mask
   mut perturb = h
   mut probes = 0
   while(probes < cap){
      def off = 16 + idx * 24
      def st = load64(d, off + 16)
      if(st == 0){ return -1 }
      if(st == 1){
         if(_dict_key_eq(load64(d, off), key)){
            return off
         }
      }
      idx = (idx * 5 + 1 + (perturb >> 5)) & mask
      perturb = perturb >> 5
      probes += 1
   }
   -1
}

fn dict_has(d, key){
   "Returns true if dictionary `d` contains `key`."
   if(!is_dict(d)){ panic("dict_has called on non-dictionary") }
   _dict_find_off(d, key) >= 0
}

fn dict_get(d, key, default=0){
   "Returns the value for `key` or `default` if missing."
   if(!is_dict(d)){ panic("dict_get called on non-dictionary") }
   def off = _dict_find_off(d, key)
   if(off >= 0){ return load64(d, off + 8) }
   default
}

fn dict_clone(d){
   "Returns a shallow clone of dictionary `d`."
   if(!is_dict(d)){ panic("dict_clone called on non-dictionary") }
   mut out = _dict_new(load64(d, 8))
   def cap = load64(d, 8)
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(load64(d, off + 16) == 1){
         out = _dict_insert(out, load64(d, off), load64(d, off + 8))
      }
      i += 1
   }
   out
}

fn dict_del(d, key){
   "Removes `key` from dictionary `d` and returns the (possibly rebuilt) dictionary."
   if(!is_dict(d)){ panic("dict_del called on non-dictionary") }
   if(_dict_find_off(d, key) < 0){ return d }
   mut out = _dict_new(load64(d, 8))
   def cap = load64(d, 8)
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(load64(d, off + 16) == 1 && !_dict_key_eq(load64(d, off), key)){
         out = _dict_insert(out, load64(d, off), load64(d, off + 8))
      }
      i += 1
   }
   free(d)
   out
}

fn dict_merge(dst, src){
   "Merges `src` into `dst` (overwriting duplicate keys). Returns merged dictionary."
   if(!is_dict(dst)){ panic("dict_merge destination must be dictionary") }
   if(!is_dict(src)){ panic("dict_merge source must be dictionary") }
   def cap = load64(src, 8)
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(load64(src, off + 16) == 1){
         dst = dict_set(dst, load64(src, off), load64(src, off + 8))
      }
      i += 1
   }
   dst
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
      if(st == 1){
         def pair = _dict_pair(load64(d, off), load64(d, off + 8))
         store64(out, pair, 16 + idx * 8)
         store64(out, idx + 1, 0)
         idx += 1
      }
      i += 1
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
      if(st == 1){
         store64(out, load64(d, off), 16 + idx * 8)
         store64(out, idx + 1, 0)
         idx += 1
      }
      i += 1
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
      if(st == 1){
         store64(out, load64(d, off + 8), 16 + idx * 8)
         store64(out, idx + 1, 0)
         idx += 1
      }
      i += 1
   }
   out
}

if(comptime{__main()}){
    use std.core *
    use std.core.dict *
    use std.core.error *

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
    d = dict_del(d, "missing")
    assert(dict_len(d) == 3, "dict_del missing no-op")

    def ks = dict_keys(d)
    def vs = dict_values(d)
    def it = dict_items(d)
    assert(len(ks) == 3, "dict_keys len")
    assert(len(vs) == 3, "dict_values len")
    assert(len(it) == 3, "dict_items len")

    print("✓ std.core.dict tests passed")
}
