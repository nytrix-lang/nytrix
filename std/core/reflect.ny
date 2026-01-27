;; Keywords: core reflect
;; Core Reflect module.

use std.core.error *
use std.str *
use std.str.str *
use std.str.bytes *
use std.math.bigint *
use std.core.primitives *
use std.core.dict *
use std.core *
use std.str.io *
module std.core.reflect (len, contains, type, typeof, list_eq, dict_eq, set_eq, eq, repr, hash, globals, items, keys, values, get, set, set_idx, slice, append, pop, extend, to_str)

fn len(x){
   "Returns the number of elements in a collection or the length of a string.
   - For **str**: number of bytes.
   - For **list/tuple/dict/set**: number of items.
   - For **bytes**: buffer size."
   if(__eq(x, 0)){ return 0 }
   if(is_list(x)){ return load64(x, 0) }
   if(is_tuple(x)){ return load64(x, 0) }
   if(is_dict(x)){
      ; Dict header: [Tag at -8 | Count at 0 | Capacity at 8 | Entries...]
      return load64(x, 0)
   }
   if(is_set(x)){ return load64(x, 0) }
   if(is_bytes(x)){ return bytes_len(x) }
   if(is_bigint(x)){ return 0 }
   if(is_str(x)){ return str_len(x) }
   return 0
}

fn contains(container, item){
   "Returns **true** if `item` exists within `container`.
   - **set/dict**: checks for key existence.
   - **list**: checks for value presence.
   - **str**: checks for substring presence."
   if(!container){ return false }
   ; Handle sets (dicts with tag 102)
   if(is_set(container)){
      def cap = load64(container, 8)
      mut h = hash(item)
      def mask = cap - 1
      mut idx = h & mask
      mut perturb = h
      mut probes = 0
      while(probes < cap){
         def off = 16 + idx * 24
         def st = load64(container, off + 16)
         if(__eq(st, 0)){ return false }
         if(__eq(st, 1)){
            if(eq(load64(container, off), item)){ return true }
         }
         idx = (idx * 5 + 1 + (perturb >> 5)) & mask
         perturb = perturb >> 5
         probes = probes + 1
      }
      return false
   }
   ; Handle dicts
   if(is_dict(container)){
      def cap = load64(container, 8)
      mut h = hash(item)
      def mask = cap - 1
      mut idx = h & mask
      mut perturb = h
      mut probes = 0
      while(probes < cap){
         def off = 16 + idx * 24
         def st = load64(container, off + 16)
         if(__eq(st, 0)){ return false }
         if(__eq(st, 1)){
            if(eq(load64(container, off), item)){ return true }
         }
         idx = (idx * 5 + 1 + (perturb >> 5)) & mask
         perturb = perturb >> 5
         probes = probes + 1
      }
      return false
   }
   ; Handle lists
   if(is_list(container)){
      mut i = 0
      def n = load64(container, 0)
      while(i < n){
         if(eq(load64(container, 16 + i * 8), item)){ return true }
         i = i + 1
      }
      return false
   }
   ; Handle strings
   if(is_str(container)){
      return find(container, item) >= 0
   }
   return false
}

fn type(x){
   "Returns a string representing the **tag-type** of Nytrix value `x`.
   Return values: `none`, `int`, `float`, `str`, `list`, `dict`, `set`, `tuple`, `bytes`, `bigint`, `bool`, `ptr`, `unknown`."
   ; None
   if(__eq(x, 0)){ return "none" }
   ; Check if it's a tagged integer
   if(is_int(x)){ return "int" }
   ; Check if pointer
   if(is_ptr(x)){
      if(is_list(x)){ return "list" }
      if(is_dict(x)){ return "dict" }
      if(is_set(x)){ return "set" }
      if(is_tuple(x)){ return "tuple" }
      if(is_str(x)){ return "str" }
      if(is_bytes(x)){ return "bytes" }
      if(is_bigint(x)){ return "bigint" }
      if(is_float(x)){ return "float" }
      return "ptr"
   }
   ; Not none, not int, not ptr -> must be bool (2 or 4)
   if(__eq(x, true) || __eq(x, false)){ return "bool" }
   return "unknown"
}

fn typeof(x){
   "Alias for `type`."
   return type(x)
}

fn list_eq(a,b){
   "Performs deep structural equality comparison for two lists."
   def na = load64(a, 0)
   def nb = load64(b, 0)
   if(!eq(na, nb)){ return false }
   mut i = 0
   while(i < na){
      def va = load64(a, 16 + i * 8)
      def vb = load64(b, 16 + i * 8)
      if(eq(eq(va, vb), false)){ return false }
      i = i + 1
   }
   return true
}

fn dict_eq(a,b){
   "Performs deep structural equality comparison for two dictionaries."
   if(!eq(len(a), len(b))){ return false }
   def its = items(a)
   mut i=0
   def n=load64(its, 0)
   while(i<n){
      def p = load64(its, 16 + i * 8)
      if(eq(eq(dict_get(b, load64(p, 16), 0xdeadbeef), load64(p, 24)), false)){ return false }
      i=i+1
   }
   return true
}

fn set_eq(a,b){
   "Performs deep structural equality comparison for two sets."
   if(!eq(len(a), len(b))){ return false }
   def its = items(a)
   mut i=0
   def n=load64(its, 0)
   while(i<n){
      def p = load64(its, 16 + i * 8)
      if(eq(contains(b, load64(p, 16)), false)){ return false }
      i=i+1
   }
   return true
}

fn eq(a, b){
   "Structural equality operator. Compares values by content (strings/collections) or identity (primitives)."
   if(__eq(a, b)){ return true }
   if(!is_ptr(a)){ return false }
   if(!is_ptr(b)){ return false }
   def ta = type(a)
   def tb = type(b)
   if(!_str_eq(ta, tb)){ return false }
   if(_str_eq(ta, "list")){ return list_eq(a, b) }
   if(_str_eq(ta, "tuple")){ return list_eq(a, b) }
   if(_str_eq(ta, "dict")){ return dict_eq(a, b) }
   if(_str_eq(ta, "set")){ return set_eq(a, b) }
   if(_str_eq(ta, "float")){ return __flt_eq(a, b) }
   if(_str_eq(ta, "bigint")){ return bigint_eq(a, b) }
   if(_str_eq(ta, "str")){ return _str_eq(a, b) }
   return false
}


fn repr(x){
   "Returns a **developer-friendly** string representation of `x`, suitable for debugging. Strings are quoted and collections are expanded recursively."
   def t = type(x)
   if(_str_eq(t, "none")){ return "none" }
   if(_str_eq(t, "bool")){
      if(__eq(x, true)){ return "true" }
      return "false"
   }
   if(_str_eq(t, "list")){
      def n = load64(x, 0)
      mut out = "["
      mut i=0
      while(i<n){
         out = f"{out}{repr(load64(x, 16 + i * 8))}"
         if(i+1<n){ out = f"{out}," }
         i=i+1
      }
      return f"{out}]"
   }
   if(_str_eq(t, "dict")){
      def its = items(x)
      mut out = "{"
      mut i=0
      def n=load64(its, 0)
      while(i<n){
         def p = load64(its, 16 + i * 8)
         out = f"{out}{repr(load64(p, 16))}:{repr(load64(p, 24))}"
         if(i+1<n){ out = f"{out}," }
         i=i+1
      }
      return f"{out}}"
   }
   if(_str_eq(t, "set")){
      def its = items(x)
      mut out = "{"
      mut i=0
      def n=load64(its, 0)
      while(i<n){
         def p = load64(its, 16 + i * 8)
         out = f"{out}{repr(load64(p, 16))}"
         if(i+1<n){ out = f"{out}," }
         i=i+1
      }
      return f"{out}}"
   }
   if(_str_eq(t, "bytes")){ return f"<bytes {bytes_len(x)}>" }
   if(_str_eq(t, "float")){ return to_str(x) }
   if(_str_eq(t, "bigint")){ return bigint_to_str(x) }
   if(_str_eq(t, "str")){ return f"\"{x}\"" }
   if(_str_eq(t, "int")){ return to_str(x) }
   if(_str_eq(t, "ptr")){ return f"<ptr {x}>" }
   return to_str(x)
}

fn hash(x){
   "Returns a **64-bit FNV-1a hash** of value `x`. Currently supports integers and strings."
   def t = type(x)
   if(_str_eq(t, "int")){ return x }
   if(_str_eq(t, "str")){
      mut h = 14695981039346656037
      mut i = 0
      def n = str_len(x)
      while(i < n){
         h = (h ^ load8(x, i)) * 1099511628211
         i = i + 1
      }
      return h
   }
   return 0
}

fn globals(){
   "Returns a dictionary containing all currently defined global variables."
   return __globals()
}

fn items(x){
   "Generic item iterator. Returns a list of `[index/key, value]` pairs."
   case type(x) {
      "dict" -> {
         dict_items(x)
      }
      "set"  -> {
         def its = dict_items(x)
         mut out = list(8)
         mut i = 0  def n = list_len(its)
         while(i < n){
             out = append(out, get(get(its, i), 0))
            i += 1
         }
         out
      }
      "list", "tuple", "str" -> {
         mut out = list(8)
         def n = len(x)
         mut i = 0
         while(i < n){
             out = append(out, [i, get(x, i)])
            i += 1
         }
         out
      }
      _ -> list(0)
   }
}

fn keys(x){
   "Generic key iterator. Returns keys or indices for the given collection."
   case type(x) {
      "dict" -> {
         dict_keys(x)
      }
      "set"  -> items(x)
      "list", "tuple", "str" -> {
         mut out = list(8)
         def n = len(x)
         mut i = 0
         while(i < n){  out = append(out, i)  i += 1 }
         out
      }
      _ -> list(0)
   }
}

fn values(x){
   "Generic value iterator for all collection types."
   case type(x) {
      "dict" -> {
         dict_values(x)
      }
      "set"  -> items(x)
      "list", "tuple", "str" -> {
         mut out = list(8)
         def n = len(x)
         mut i = 0
         while(i < n){  out = append(out, get(x, i))  i += 1 }
         out
      }
      _ -> list(0)
   }
}

fn get(obj, key, default=0){
   "Generic element retriever. Handles indexing for strings, lists, dicts, and tuples.
    - `obj`: Collection (str, list, dict, tuple)
    - `key`: Index or Key
    - `default`: Value to return if key/index not found (default 0).
   "
   def t = type(obj)
   if(eq(t, "str")){
      def n = len(obj)
      if(key < 0){ key += n }
      if(key < 0 || key >= n){ default }
      else {
       use std.str.str *
       str_slice(obj, key, key + 1)
      }
   }
   elif(eq(t, "dict")){ dict_get(obj, key, default) }
   elif(eq(t, "list") || eq(t, "tuple")){
      def n = len(obj)
      if(key < 0){ key += n }
      if(key < 0 || key >= n){ default }
      else { load64(obj, 16 + key * 8) }
   }
   else { default }
}

fn set_idx(obj, key, val){
   "Generic element setter. Supported for dicts and lists. Returns the object or 0 on failure."
   def t = type(obj)
   if(eq(t, "dict")){ dict_set(obj, key, val) }
   elif(eq(t, "list")){
      def n = len(obj)
      if(key < 0){ key += n }
      if(key < 0 || key >= n){ 0 }
      else { 
         store64(obj, val, 16 + key * 8)
         val 
      }
   }
   else { 0 }
}

fn set(obj, key, val){
   "Alias for set_idx."
   set_idx(obj, key, val)
}

fn slice(obj, start, stop, step=1){
   "Generic **slice** operation for strings and lists."
   def t = type(obj)
   if(eq(t, "str")){
       str_slice(obj, start, stop, step)
   }
   elif(eq(t, "list")){
       def n = len(obj)
       if(start < 0){ start = n + start }
       if(stop < 0){ stop = n + stop }
       if(step > 0){
        if(start < 0){ start = 0 }
        if(stop > n){ stop = n }
        if(start >= stop){ return list(0) }
       } else {
        if(start >= n){ start = n - 1 }
        if(stop < -1){ stop = -1 }
        if(start <= stop){ return list(0) }
       }
       mut out = list(8)
       mut i = start
       if(step > 0){
        while(i < stop){
           out = append(out, get(obj, i))
           i += step
        }
       } else {
        while(i > stop){
           out = append(out, get(obj, i))
           i += step
        }
       }
       out
   }
   else { 0 }
}

fn append(lst, v){
   "Appends value `v` to the end of list `lst`. Returns the (possibly reallocated) list ptr."
   if(!is_list(lst)){ lst }
   else {
     mut out = lst
     def tag = load64(out, -8)
     def n = load64(out, 0)
     def cap = load64(out, 8)
     if(n >= cap){
       def newcap = eq(cap, 0) ? 8 : (cap * 2)
       def newp = list(newcap)
       store64(newp, tag, -8)
       mut i = 0
       while(i < n){ 
          store64(newp, load64(out, 16 + i * 8), 16 + i * 8)  
          i += 1 
       }
       free(out)
       out = newp
     }
     store64(out, v, 16 + n * 8)
     store64(out, n + 1, 0)
     out
   }
}

fn pop(lst){
   "Removes and returns the last element from list `lst`. Returns `0` if empty."
   if(!is_list(lst)){ return 0 }
   else {
    def n = load64(lst, 0)
    if(eq(n, 0)){ 0 }
    else {
      def v = get(lst, n - 1)
      store64(lst, n - 1, 0)
      v
    }
   }
}

fn extend(lst, other){
   "Appends all elements from collection `other` to the list `lst`."
   if(eq(is_list(lst), false)){ return lst }
   
   mut i = 0
   def n = len(other)
   while(i < n){
      lst = append(lst, get(other, i))
      i += 1
   }
   lst
}

fn to_str(v){
   "Converts any Nytrix value to its string representation. Handles recursive collection printing."
   if(is_list(v)){
      def n = len(v)
      mut s = "["
      mut i = 0
      while(i < n){
         s = f"{s}{to_str(get(v, i))}"
         if(i < n - 1){ s = f"{s}, " }
         i += 1
      }
      f"{s}]"
   } else {
     if(is_dict(v)){ "{...}" }
     elif(is_bytes(v)){ f"<bytes {bytes_len(v)}>" }
     else {
        if(is_ptr(v)){
           def tag = load64(v, -8)
           return f"<ptr {v} tag={tag}>"
        }
        __to_str(v)
     }
   }
}
