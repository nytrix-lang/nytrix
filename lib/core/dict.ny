;; Keywords: dict map core
;; Dictionary operations for lookup, mutation, deletion, merging, and compatibility calls.
;; References:
;; - std.core
module std.core.dict_mod(dict, dict_len, dict_read, dict_exists, dict_write, dict_remove, dict_has, dict_del, dict_pop, dict_popitem, dict_clone, dict_merge, dict_items, dict_keys, dict_values, dict_setdefault, dict_clear)
use std.core.primitives
use std.core.error

@inline
fn is_dict(any x) bool {
   "Internal: returns **true** when `x` is a dictionary object."
   def tag = runtime_tag_raw("dict")
   def got = __tagof(x)
   got == tag || got == __tag(tag)
}

@inline
fn is_str(any x) bool {
   "Internal: returns **true** when `x` is a Nytrix string object."
   __is_str_obj(x)
}

@inline
fn to_int(any v) any {
   "Internal: converts tagged integers to raw ints and leaves other values unchanged."
   __is_int(v) ? __untag(v) : v
}

@returns_owned
fn list(int cap=8) list {
   "Internal: allocates a list with initial capacity `cap`."
   __list_new(cap)
}

@inline
fn _dict_store_item(list xs, int index, any value) list {
   xs[index] = value
   xs
}

fn _pow2(int n) int {
   mut v = 1
   while(v < n){ v = v << 1 }
   v
}

@inline
fn _dict_str_eq(any a, any b) bool { __str_eq(a, b) }

@inline
fn _dict_key_eq(any a, any b) bool {
   if(is_str(a) && is_str(b)){ return _dict_str_eq(a, b) }
   return(a == b)
}

@inline
fn _dict_hash(any x) int {
   if(is_int(x)){ return x }
   if(!x){ return 0 }
   if(is_str(x)){ return __str_hash(x) }
   if(__is_float_obj(x)){ return __flt_hash(x) }
   if(is_ptr(x)){ return band(bshr(to_int(x), 3), 2147483647) }
   return 0
}

@returns_owned
fn _dict_new(int cap) dict {
   def size = 16 + cap * 24
   mut p = __malloc(size)
   __memset(p, 0, size)
   __store64_idx(p, -8, runtime_tag_raw("dict"))
   __store64_idx(p, 0, 0)
   __store64_idx(p, 8, cap)
   p
}

@returns_owned
fn dict(int cap=8) dict {
   "Creates a new empty dictionary."
   _dict_new(_pow2(cap))
}

@inline
fn dict_len(dict d) int {
   "Returns the number of entries in dictionary `d`."
   if(!is_dict(d)){ return 0 }
   __load64_idx(d, 0)
}

@inline
fn _dict_find_off(dict d, any key) int {
   def cap = __load64_idx(d, 8)
   if(cap <= 0){ return -1 }
   def mask = cap - 1
   def h = _dict_hash(key)
   mut idx = h & mask
   mut first_tomb = -1
   mut i = 0
   while(i < cap){
      def off = 16 + idx * 24
      def state = __load64_idx(d, off + 16)
      if(!state){
         if(first_tomb >= 0){ return first_tomb }
         return off
      }
      if(state == 1 && _dict_key_eq(__load64_idx(d, off), key)){ return off }
      if(state == 2 && first_tomb < 0){ first_tomb = off }
      idx = (idx + 1) & mask
      i += 1
   }
   return first_tomb
}

@returns_owned
fn _dict_resize(dict d) dict {
   def old_cap = __load64_idx(d, 8)
   def new_cap = old_cap < 8 ? 8 : old_cap * 2
   mut nd = _dict_new(new_cap)
   mut i = 0
   while(i < old_cap){
      def off = 16 + i * 24
      if(__load64_idx(d, off + 16) == 1){ dict_write(nd, __load64_idx(d, off), __load64_idx(d, off + 8)) }
      i += 1
   }
   nd
}

fn dict_write(dict d, any key, any val) dict {
   "Inserts or updates a key/value pair in dictionary `d`."
   if(!is_dict(d)){ return d }
   def tc = __load64_idx(d, 0)
   def tca = __load64_idx(d, 8)
   mut off = _dict_find_off(d, key)
   if(off >= 0 && __load64_idx(d, off + 16) == 1){
      __store64_idx(d, off + 8, val)
      return d
   }
   if(off < 0 || (tc + 1) * 2 > tca){
      mut nd = _dict_resize(d)
      return dict_write(nd, key, val)
   }
   __store64_idx(d, off, key)
   __store64_idx(d, off + 8, val)
   __store64_idx(d, off + 16, 1)
   __store64_idx(d, 0, tc + 1)
   d
}

@inline
fn dict_read(dict d, any key, any default=0) any {
   "Retrieves the value for `key` in `d`, or returns `default` if not found."
   if(!is_dict(d)){ return default }
   def off = _dict_find_off(d, key)
   if(off < 0 || __load64_idx(d, off + 16) != 1){ return default }
   __load64_idx(d, off + 8)
}

@inline
fn dict_exists(dict d, any key) bool {
   "Returns **true** if `key` exists in dictionary `d`."
   if(!is_dict(d)){ return false }
   def off = _dict_find_off(d, key)
   if(off < 0 || __load64_idx(d, off + 16) != 1){ return false }
   true
}

@inline
fn dict_has(dict d, any key) bool {
   "Compatibility bridge for old free helper calls. Prefer `d.contains(key)`."
   dict_exists(d, key)
}

@inline
fn dict_remove(dict d, any key) dict {
   "Removes `key` from dictionary `d`. Returns the dictionary."
   if(!is_dict(d)){ return d }
   def off = _dict_find_off(d, key)
   if(off >= 0 && __load64_idx(d, off + 16) == 1){
      __store64_idx(d, off, 0)
      __store64_idx(d, off + 8, 0)
      __store64_idx(d, off + 16, 2)
      __store64_idx(d, 0, __load64_idx(d, 0) - 1)
   }
   d
}

@inline
fn dict_del(dict d, any key) dict {
   "Compatibility bridge for old free helper calls. Prefer `d.delete(key)`."
   dict_remove(d, key)
}

fn dict_pop(dict d, any key, any default=0) any {
   "Removes and returns the value for `key`, or `default` if not found."
   if(!is_dict(d)){ return default }
   def off = _dict_find_off(d, key)
   if(off >= 0 && __load64_idx(d, off + 16) == 1){
      def val = __load64_idx(d, off + 8)
      __store64_idx(d, off, 0)
      __store64_idx(d, off + 8, 0)
      __store64_idx(d, off + 16, 2)
      __store64_idx(d, 0, __load64_idx(d, 0) - 1)
      return val
   }
   default
}

fn dict_popitem(dict d) any {
   "Removes and returns the last inserted [key, value] pair, or 0 if empty."
   if(!is_dict(d)){ return 0 }
   def cap = __load64_idx(d, 8)
   mut i = cap - 1
   while(i >= 0){
      def off = 16 + i * 24
      if(__load64_idx(d, off + 16) == 1){
         def key = __load64_idx(d, off)
         def val = __load64_idx(d, off + 8)
         __store64_idx(d, off, 0)
         __store64_idx(d, off + 8, 0)
         __store64_idx(d, off + 16, 2)
         __store64_idx(d, 0, __load64_idx(d, 0) - 1)
         return [key, val]
      }
      i -= 1
   }
   0
}

fn dict_setdefault(dict d, any key, any default=0) any {
   "Returns the value for `key`, or sets and returns `default` if not found."
   if(!is_dict(d)){ return default }
   if(dict_exists(d, key)){ return dict_read(d, key, default) }
   d = dict_write(d, key, default)
   default
}

@returns_owned
fn dict_clone(dict d) dict {
   "Creates a shallow copy of dictionary `d`."
   if(!is_dict(d)){ return d }
   def cap = __load64_idx(d, 8)
   mut nd = _dict_new(cap)
   __store64_idx(nd, 0, __load64_idx(d, 0))
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(__load64_idx(d, off + 16) == 1){
         __store64_idx(nd, off, __load64_idx(d, off))
         __store64_idx(nd, off + 8, __load64_idx(d, off + 8))
         __store64_idx(nd, off + 16, 1)
      }
      i += 1
   }
   nd
}

fn dict_clear(dict d) dict {
   "Removes all entries from dictionary `d`."
   if(!is_dict(d)){ return d }
   def cap = __load64_idx(d, 8)
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      __store64_idx(d, off, 0)
      __store64_idx(d, off + 8, 0)
      __store64_idx(d, off + 16, 0)
      i += 1
   }
   __store64_idx(d, 0, 0)
   d
}

fn dict_merge(dict dst, dict src) dict {
   "Merges `src` into `dst` (overwriting duplicate keys). Returns merged dictionary."
   if(!is_dict(dst) || !is_dict(src)){ return dst }
   def cap = __load64_idx(src, 8)
   mut i = 0
   while(i < cap){
      def off = 16 + i * 24
      if(__load64_idx(src, off + 16) == 1){ dst = dict_write(dst, __load64_idx(src, off), __load64_idx(src, off + 8)) }
      i += 1
   }
   dst
}

@inline
@returns_owned
fn _dict_pair(any a, any b) list {
   mut p = list(2)
   _dict_store_item(p, 0, a)
   _dict_store_item(p, 1, b)
   __store64_idx(p, 0, 2)
   p
}

@returns_owned
fn dict_items(dict d) list {
   "Returns a list of [key, value] pairs."
   if(!is_dict(d)){ return list() }
   def n = __load64_idx(d, 0)
   mut out = list(n)
   def cap = __load64_idx(d, 8)
   mut i = 0
   mut pos = 0
   while(i < cap){
      def off = 16 + i * 24
      if(__load64_idx(d, off + 16) == 1){
         _dict_store_item(out, pos, _dict_pair(__load64_idx(d, off), __load64_idx(d, off + 8)))
         pos += 1
      }
      i += 1
   }
   __store64_idx(out, 0, pos)
   out
}

@returns_owned
fn dict_keys(dict d) list {
   "Returns a list of keys."
   if(!is_dict(d)){ return list() }
   def n = __load64_idx(d, 0)
   mut out = list(n)
   def cap = __load64_idx(d, 8)
   mut i = 0
   mut pos = 0
   while(i < cap){
      def off = 16 + i * 24
      if(__load64_idx(d, off + 16) == 1){
         _dict_store_item(out, pos, __load64_idx(d, off))
         pos += 1
      }
      i += 1
   }
   __store64_idx(out, 0, pos)
   out
}

@returns_owned
fn dict_values(dict d) list {
   "Returns a list of values."
   if(!is_dict(d)){ return list() }
   def n = __load64_idx(d, 0)
   mut out = list(n)
   def cap = __load64_idx(d, 8)
   mut i = 0
   mut pos = 0
   while(i < cap){
      def off = 16 + i * 24
      if(__load64_idx(d, off + 16) == 1){
         _dict_store_item(out, pos, __load64_idx(d, off + 8))
         pos += 1
      }
      i += 1
   }
   __store64_idx(out, 0, pos)
   out
}

#main {
   fn _dict_check(bool cond, str msg) int {
      if(!cond){ __panic(msg) }
      0
   }
   mut d = dict(4)
   _dict_check(dict_len(d) == 0 && !dict_has(d, "a"), "dict empty")
   d = dict_write(d, "a", 1)
   d = dict_write(d, "b", 2)
   d = dict_write(d, "c", 3)
   _dict_check(dict_len(d) == 3 && dict_has(d, "b") && dict_read(d, "b", 0) == 2 && dict_read(d, "x", 77) == 77, "dict write/read")
   mut copy = dict_clone(d)
   copy = dict_write(copy, "b", 22)
   _dict_check(dict_read(d, "b", 0) == 2 && dict_read(copy, "b", 0) == 22, "dict clone isolates writes")
   mut other = dict(4)
   other = dict_write(other, "b", 200)
   other = dict_write(other, "d", 4)
   d = dict_merge(d, other)
   _dict_check(dict_len(d) == 4 && dict_read(d, "b", 0) == 200 && dict_read(d, "d", 0) == 4, "dict merge")
   d = dict_del(d, "b")
   _dict_check(!dict_has(d, "b") && dict_len(d) == 3, "dict delete")
   _dict_check(dict_keys(d).contains("a") && dict_values(d).contains(4) && dict_items(d).len == 3, "dict views")
   mut fd = dict(8)
   def half_a = 0.5
   def half_b = 0.25 + 0.25
   fd[half_a] = "half"
   _dict_check(dict_read(fd, half_b, "") == "half", "dict float keys")
   print("✓ std.core.dict_mod self-test passed")
}
