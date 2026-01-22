;; Keywords: core reflect
;; Core Reflect module.

use std.core.error
use std.strings.str
use std.strings.bytes
use std.math.bigint
module std.core.reflect (len, contains, type, list_eq, dict_eq, set_eq, eq,  repr, hash, globals)

fn len(x){
   "Returns the number of elements in a collection or the length of a string.
   - For **str**: number of bytes.
   - For **list/tuple/dict/set**: number of items.
   - For **bytes**: buffer size."
   if(x == 0){ return 0 }
   if(is_list(x)){ return __load64_idx(x, 0) }
   if(is_tuple(x)){ return __load64_idx(x, 0) }
   if(is_dict(x)){
      ; Dict header: [Tag at -8 | Count at 0 | Capacity at 8 | Entries...]
      return __load64_idx(x, 0)
   }
   if(is_set(x)){ return __load64_idx(x, 0) }
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
      def cap = __load64_idx(container, 8)
      def h = hash(item)
      def mask = cap - 1
      def idx = h & mask
      def perturb = h
      def probes = 0
      while(probes < cap){
         def off = 16 + idx * 24
         def st = __load64_idx(container, off + 16)
         if(st == 0){ return false }
         if(st == 1){
            if(eq(__load64_idx(container, off), item)){ return true }
         }
         idx = (idx * 5 + 1 + (perturb >> 5)) & mask
         perturb = perturb >> 5
         probes = probes + 1
      }
      return false
   }
   ; Handle dicts
   if(is_dict(container)){
      def cap = __load64_idx(container, 8)
      def h = hash(item)
      def mask = cap - 1
      def idx = h & mask
      def perturb = h
      def probes = 0
      while(probes < cap){
         def off = 16 + idx * 24
         def st = __load64_idx(container, off + 16)
         if(st == 0){ return false }
         if(st == 1){
            if(eq(__load64_idx(container, off), item)){ return true }
         }
         idx = (idx * 5 + 1 + (perturb >> 5)) & mask
         perturb = perturb >> 5
         probes = probes + 1
      }
      return false
   }
   ; Handle lists
   if(is_list(container)){
      def i = 0
      def n = __load64_idx(container, 0)
      while(i < n){
         if(eq(__load64_idx(container, 16 + i * 8), item)){ return true }
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
   if(x == 0){ return "none" }
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
   if(x == true || x == false){ return "bool" }
   return "unknown"
}

fn list_eq(a,b){
   "Performs deep structural equality comparison for two lists."
   if(__load64_idx(a, 0) != __load64_idx(b, 0)){ return false }
   def i = 0
   def n = __load64_idx(a, 0)
   while(i < n){
      if(eq(__load64_idx(a, 16 + i * 8), __load64_idx(b, 16 + i * 8)) == false){ return false }
      i = i + 1
   }
   return true
}

fn dict_eq(a,b){
   "Performs deep structural equality comparison for two dictionaries."
   if(len(a)!=len(b)){ return false }
   def its = items(a)
   def i=0
   def n=__load64_idx(its, 0)
   while(i<n){
      def p = __load64_idx(its, 16 + i * 8)
      if(eq(dict_get(b, __load64_idx(p, 16), 0xdeadbeef), __load64_idx(p, 24)) == false){ return false }
      i=i+1
   }
   return true
}

fn set_eq(a,b){
   "Performs deep structural equality comparison for two sets."
   if(len(a)!=len(b)){ return false }
   def its = items(a)
   def i=0
   def n=__load64_idx(its, 0)
   while(i<n){
      def p = __load64_idx(its, 16 + i * 8)
      if(contains(b, __load64_idx(p, 16)) == false){ return false }
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
   if(!__eq(ta, tb)){ return false }
   if(__eq(ta, "list")){ return list_eq(a, b) }
   if(__eq(ta, "dict")){ return dict_eq(a, b) }
   if(__eq(ta, "set")){ return set_eq(a, b) }
   if(__eq(ta, "float")){ return __flt_eq(a, b) }
   if(__eq(ta, "bigint")){ return bigint_eq(a, b) }
   if(__eq(ta, "str")){ return _str_eq(a, b) }
   return _str_eq(a, b)
}


fn repr(x){
   "Returns a **developer-friendly** string representation of `x`, suitable for debugging. Strings are quoted and collections are expanded recursively."
   def t = type(x)
   return case t {
      "none"   -> "none"
      "bool"   -> case x { true -> "true" _ -> "false" }
      "list" -> {
         def n = __load64_idx(x, 0)
         def out = "["
         def i=0
         while(i<n){
            out = f"{out}{repr(__load64_idx(x, 16 + i * 8))}"
            if(i+1<n){ out = f"{out}," }
            i=i+1
         }
         f"{out}]"
      }
      "dict" -> {
         def its = items(x)
         def out = "{"
         def i=0
         def n=__load64_idx(its, 0)
         while(i<n){
            def p = __load64_idx(its, 16 + i * 8)
            out = f"{out}{repr(__load64_idx(p, 16))}:{repr(__load64_idx(p, 24))}"
            if(i+1<n){ out = f"{out}," }
            i=i+1
         }
         f"{out}}"
      }
      "set" -> {
         def its = items(x)
         def out = "{"
         def i=0
         def n=__load64_idx(its, 0)
         while(i<n){
            def p = __load64_idx(its, 16 + i * 8)
            out = f"{out}{repr(__load64_idx(p, 16))}"
            if(i+1<n){ out = f"{out}," }
            i=i+1
         }
         f"{out}}"
      }
      "bytes"  -> f"<bytes {bytes_len(x)}>"
      "float"  -> __to_str(x)
      "bigint" -> bigint_to_str(x)
      "str"    -> f"\"{x}\""
      "int"    -> to_str(x)
      "ptr"    -> f"<ptr {x}>"
      _        -> to_str(x)
   }
}

fn hash(x){
   "Returns a **64-bit FNV-1a hash** of value `x`. Currently supports integers and strings."
   def t = type(x)
   if(t == "int"){ return x }
   if(t == "str"){
      def h = 14695981039346656037
      def i = 0
      def n = str_len(x)
      while(i < n){
         h = (h ^ __load8_idx(x, i)) * 1099511628211
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