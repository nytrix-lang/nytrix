;; Keywords: core reflect
;; Core Reflect module.

module std.core.reflect (
   len, contains, type, typeof,
   add, sub, mul, div,
   list_eq, dict_eq, set_eq, eq, repr, hash,
   globals, items, keys, values, get, set, set_idx, slice, append, pop, extend, to_str
)
use std.core.error *
use std.str *
use std.str.str *
use std.str.bytes *
use std.math.bigint *
use std.core.primitives *
use std.core.dict *
use std.core *
use std.str.io *

fn len(x){
   "Returns the number of elements in a collection or the length of a string.
   - For **str**: number of bytes.
   - For **list/tuple/dict/set**: number of items.
   - For **bytes**: buffer size.
   Returns `0` for other types."
   if(x == 0){ return 0 }
   if(is_int(x)){ return 0 }
   if(is_str(x)){ return str_len(x) }
   if(!is_ptr(x)){ return 0 }
   def kind = __tagof(x)
   case kind {
      100 -> load64(x, 0) ; List
      103 -> load64(x, 0) ; Tuple
      101 -> load64(x, 0) ; Dict
      102 -> load64(x, 0) ; Set
      122 -> bytes_len(x) ; Bytes
      _   -> 0
   }
}

fn contains(container, item){
   "Returns **true** if `item` exists within `container`.
   - **set/dict**: checks for key existence.
   - **list/tuple**: checks for value presence.
   - **str**: checks for substring presence."
   if(!container){ return false }
   if(is_set(container) || is_dict(container)){
      def cap = load64(container, 8)
      mut h = hash(item)
      def mask = cap - 1
      mut idx = h & mask
      mut perturb = h
      mut probes = 0
      while(probes < cap){
         def off = 16 + idx * 24
         def st = load64(container, off + 16)
         if(st == 0){ return false }
         if(st == 1){
            if(load64(container, off) == item){ return true }
         }
         idx = (idx * 5 + 1 + (perturb >> 5)) & mask
         perturb = perturb >> 5
         probes += 1
      }
      return false
   }
   if(is_list(container) || is_tuple(container)){
      mut i = 0
      def n = load64(container, 0)
      while(i < n){
         if(load64(container, 16 + i * 8) == item){ return true }
         i += 1
      }
      return false
   }
   if(is_str(container)){ return find(container, item) >= 0 }
   return false
}

fn type(x){
   "Returns a string representing the **tag-type** of Nytrix value `x`.
   Return values: `none`, `int`, `float`, `str`, `list`, `dict`, `set`, `tuple`, `bytes`, `bigint`, `bool`, `ptr`, `unknown`."
   if(x == true || x == false){ return "bool" }
   if(is_int(x)){ return "int" }
   if(is_str(x)){ return "str" }
   if(__is_ny_obj(x)){
      def tag = __tagof(x)
      return case tag {
         100 -> "list"
         101 -> "dict"
         102 -> "set"
         103 -> "tuple"
         110 -> "float"
         122 -> "bytes"
         130 -> "bigint"
         120 -> "str"
         _   -> "ptr"
      }
   }
   if(is_ptr(x)){ return "ptr" }
   if(!x){ return "none" }
   return "unknown"
}

fn add(a, b){
   "Generic addition.
   - **list/tuple + list/tuple**: element-wise sum (min length).
   - Other types: delegates to builtin `__add` (ints, floats, strings, ptr math)."
   if(_is_list_or_tuple(a) && _is_list_or_tuple(b)){
      return _list_zip2(a, b, 0)
   }
   __add(a, b)
}

fn sub(a, b){
   "Generic subtraction with list support.
   - **list/tuple - list/tuple**: element-wise difference (min length).
   - Other types: delegates to builtin `__sub`."
   if(_is_list_or_tuple(a) && _is_list_or_tuple(b)){
      return _list_zip2(a, b, 1)
   }
   __sub(a, b)
}

fn mul(a, b){
   "Generic multiplication.
   - **mat4 * mat4**: matrix product.
   - **mat4 * vec4**: matrix-vector product.
   - **list/tuple * list/tuple**: element-wise product (min length).
   - **list/tuple * scalar**: scale each element.
   - Other types: delegates to builtin `__mul`."
   if(_is_list_or_tuple(a)){
      if(_is_mat4(a)){
         if(_is_list_or_tuple(b) && len(b) == 16){ return _mat4_mul(a, b) }
         if(_is_list_or_tuple(b) && len(b) == 4){ return _mat4_mul_vec4(a, b) }
      }
      if(is_int(b) || is_float(b)){ return _list_scale(a, b, 0) }
      if(_is_list_or_tuple(b)){ return _list_zip2(a, b, 2) }
   }
   if(_is_list_or_tuple(b) && (is_int(a) || is_float(a))){ return _list_scale(b, a, 0) }
   __mul(a, b)
}

fn div(a, b){
   "Generic division.
   - **list/tuple / list/tuple**: element-wise division (min length).
   - **list/tuple / scalar**: divide each element by scalar.
   - Other types: delegates to builtin `__div`."
   if(_is_list_or_tuple(a) && _is_list_or_tuple(b)){
      return _list_zip2(a, b, 3)
   }
   if(_is_list_or_tuple(a) && (is_int(b) || is_float(b))){ return _list_scale(a, b, 1) }
   __div(a, b)
}

fn _is_list_or_tuple(x){
   "Internal: fast list/tuple check that avoids deep runtime object validation."
   if(!__is_ny_obj(x)){ return false }
   def tag = __tagof(x)
   return tag == 100 || tag == 103
}

fn _list_like(n){
   "Internal: allocates a list container with length `n`."
   list(n)
}

fn _list_zip2(a, b, op){
   "Internal: element-wise list/tuple operations (op: 0 add, 1 sub, 2 mul, 3 div)."
   def na = len(a)
   def nb = len(b)
   def n = (na < nb) ? na : nb
   def out_tag = (is_tuple(a) && is_tuple(b)) ? 103 : 100
   mut out = _list_like(n)
   mut i = 0
   while(i < n){
      def x = get(a, i, 0)
      def y = get(b, i, 0)
      if(op == 0){ out = append(out, x + y) }
      else if(op == 1){ out = append(out, x - y) }
      else if(op == 2){ out = append(out, x * y) }
      else { out = append(out, x / y) }
      i += 1
   }
   if(out_tag == 103){ store64(out, 103, -8) }
   return out
}

fn _list_scale(a, s, op){
   "Internal: scales list/tuple `a` by scalar `s` (op: 0 mul, 1 div)."
   def n = len(a)
   def out_tag = is_tuple(a) ? 103 : 100
   mut out = _list_like(n)
   mut i = 0
   while(i < n){
      def x = get(a, i, 0)
      if(op == 0){ out = append(out, x * s) }
      else { out = append(out, x / s) }
      i += 1
   }
   if(out_tag == 103){ store64(out, 103, -8) }
   return out
}

fn _is_mat4(x){
   "Internal: returns true if `x` looks like a 4x4 matrix list."
   return _is_list_or_tuple(x) && len(x) == 16
}

fn _mat4_mul(a, b){
   "Internal: multiplies two 4x4 matrices."
   mut out = list(16)
   mut r = 0
   while(r < 4){
      mut c = 0
      while(c < 4){
         mut s = 0
         mut k = 0
         while(k < 4){
            s = s + get(a, r * 4 + k, 0) * get(b, k * 4 + c, 0)
            k += 1
         }
         out = append(out, s)
         c += 1
      }
      r += 1
   }
   return out
}

fn _mat4_mul_vec4(m, v){
   "Internal: multiplies 4x4 matrix `m` by 4D vector `v`."
   mut out = list(4)
   mut r = 0
   while(r < 4){
      mut s = 0
      mut c = 0
      while(c < 4){
         s = s + get(m, r * 4 + c, 0) * get(v, c, 0)
         c += 1
      }
      out = append(out, s)
      r += 1
   }
   return out
}

fn typeof(x){
   "Alias for `type`."
   return type(x)
}

fn list_eq(a,b){
   "Performs deep structural equality comparison for two lists."
   def na = load64(a, 0)
   def nb = load64(b, 0)
   if(!(na == nb)){ return false }
   mut i = 0
   while(i < na){
      def va = load64(a, 16 + i * 8)
      def vb = load64(b, 16 + i * 8)
      if(!(va == vb)){ return false }
      i += 1
   }
   return true
}

fn dict_eq(a,b){
   "Performs deep structural equality comparison for two dictionaries."
   if(!(len(a) == len(b))){ return false }
   def its = items(a)
   mut i=0
   def n=load64(its, 0)
   while(i<n){
      def p = load64(its, 16 + i * 8)
      if(!(dict_get(b, load64(p, 16), 0xdeadbeef) == load64(p, 24))){ return false }
      i=i+1
   }
   return true
}

fn set_eq(a,b){
   "Performs deep structural equality comparison for two sets."
   if(!(len(a) == len(b))){ return false }
   def its = items(a)
   mut i=0
   def n=load64(its, 0)
   while(i<n){
      def p = load64(its, 16 + i * 8)
      if(!(contains(b, load64(p, 16)))){ return false }
      i=i+1
   }
   return true
}

fn eq(a, b){
   "Structural equality operator. Compares values by content (strings/collections) or identity (primitives)."
   if(__eq(a, b)){ return true }
   if(!__is_ny_obj(a) || !__is_ny_obj(b)){ return false }
   def ta = __tagof(a)
   def tb = __tagof(b)
   if(ta == tb){
      case ta {
         100 -> list_eq(a, b)
         103 -> list_eq(a, b)
         120 -> _str_eq(a, b)
         101 -> dict_eq(a, b)
         102 -> set_eq(a, b)
         110 -> __flt_eq(a, b)
         130 -> bigint_eq(a, b)
         _   -> false
      }
   } else {
      false
   }
}

fn repr(x){
   "Function `repr`."
   if(x == true){ return "true" }
   if(x == false){ return "false" }
   if(is_int(x)){ return __to_str(x) }
   if(!x){ return "none" }
   if(is_str(x)){ return f"\"{x}\"" }
   if(!__is_ny_obj(x)){
      return to_str(x)
   }
   def kind = __tagof(x)
   ; Check for bigint first (it's a list with special marker)
   if(kind == 100 && is_list(x) && len(x) >= 3 && get(x, 0) == 107){ return bigint_to_str(x) }
   return case kind {
      100 -> {
         def n = len(x)
         mut s = "["
         mut i = 0
         while(i < n){
            s = f"{s}{repr(get(x, i))}"
            if(i < n - 1){ s = f"{s}, " }
            i += 1
         }
         return f"{s}]"
      }
      103 -> {
         def n = len(x)
         mut s = "("
         mut i = 0
         while(i < n){
            s = f"{s}{repr(get(x, i))}"
            if(i < n - 1){ s = f"{s}, " }
            i += 1
         }
         return f"{s})"
      }
      101 -> {
         def its = items(x)
         mut s = "{"
         mut i = 0
         def n = len(its)
         while(i < n){
            def p = get(its, i)
            s = f"{s}{repr(get(p, 0))}: {repr(get(p, 1))}"
            if(i < n - 1){ s = f"{s}, " }
            i += 1
         }
         return f"{s}}}"
      }
      102 -> {
         def its = items(x)
         mut s = "{"
         mut i = 0
         while(i < len(its)){
            s = f"{s}{repr(get(its, i))}"
            if(i < len(its) - 1){ s = f"{s}, " }
            i += 1
         }
         return f"{s}}}"
      }
      110 -> to_str(x)
      122 -> f"<bytes {bytes_len(x)}>"
      130 -> bigint_to_str(x)
      _   -> f"<ptr {x} tag={__tagof(x)}>"
   }
}

fn hash(x){
   "Returns a **64-bit FNV-1a hash** of value `x`. Currently supports integers and strings."
   if(is_int(x)){ return x }
   if(is_str(x)){
      mut h = 14695981039346656037
      mut i = 0
      def n = str_len(x)
      while(i < n){
         h = (h ^ load8(x, i)) * 1099511628211
         i += 1
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
   if(is_dict(x)){
      return dict_items(x)
   }
   if(is_set(x)){
      def its = dict_items(x)
      mut out = list(8)
      mut i = 0
      def n = len(its)
      while(i < n){
         out = append(out, get(get(its, i), 0))
         i += 1
      }
      return out
   }
   if(is_list(x) || is_tuple(x) || is_str(x)){
      mut out = list(8)
      def n = len(x)
      mut i = 0
      while(i < n){
         out = append(out, [i, get(x, i)])
         i += 1
      }
      return out
   }
   return list(0)
}

fn keys(x){
   "Generic key iterator. Returns keys or indices for the given collection."
   if(is_dict(x)){
      return dict_keys(x)
   }
   if(is_set(x)){
      return items(x)
   }
   if(is_list(x) || is_tuple(x) || is_str(x)){
      mut out = list(8)
      def n = len(x)
      mut i = 0
      while(i < n){
         out = append(out, i)
         i += 1
      }
      return out
   }
   return list(0)
}

fn values(x){
   "Generic value iterator for all collection types."
   if(is_dict(x)){
      return dict_values(x)
   }
   if(is_set(x)){
      return items(x)
   }
   if(is_list(x) || is_tuple(x) || is_str(x)){
      mut out = list(8)
      def n = len(x)
      mut i = 0
      while(i < n){
         out = append(out, get(x, i))
         i += 1
      }
      return out
   }
   return list(0)
}

fn get(obj, key, default=0){
   "Generic element retriever. Handles indexing for strings, lists, dicts, and tuples.
    - `obj`: Collection (str, list, dict, tuple)
    - `key`: Index or Key
    - `default`: Value to return if key/index not found (default 0).
   "
   if(is_str(obj)){
      def n = len(obj)
      if(key < 0){ key = key + n }
      if(key < 0 || key >= n){ default }
      else {
       use std.str.str *
       str_slice(obj, key, key + 1)
      }
   }
   elif(is_dict(obj)){ dict_get(obj, key, default) }
   elif(is_list(obj) || is_tuple(obj)){
      def n = len(obj)
      if(key < 0){ key = key + n }
      if(key < 0 || key >= n){ default }
      else { load64(obj, 16 + key * 8) }
   }
   else {
       if(!obj || obj == globals()){ return default }
       return get(globals(), key, default)
    }
}

fn set_idx(obj, key, val){
   "Generic element setter. Supported for dicts and lists. Returns the object or 0 on failure."
   if(is_dict(obj)){ dict_set(obj, key, val) }
   elif(is_list(obj)){
      def n = len(obj)
      if(key < 0){ key = key + n }
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
   if(is_str(obj)){
       utf8_slice(obj, start, stop, step)
   }
   elif(is_list(obj)){
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
           i = i + step
        }
       } else {
        while(i > stop){
           out = append(out, get(obj, i))
           i = i + step
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
     def n = load64(out, 0)
     def cap = load64(out, 8)
     if(n >= cap){
       def newcap = (cap == 0) ? 8 : (cap * 2)
       def newp = list(newcap)
       store64(newp, load64(out, -8), -8)
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
    if(n == 0){ 0 }
    else {
      def v = get(lst, n - 1)
      store64(lst, n - 1, 0)
      v
    }
   }
}

fn extend(lst, other){
   "Appends all elements from collection `other` to the list `lst`."
   if(!is_list(lst)){ return lst }

   mut i = 0
   def n = len(other)
   while(i < n){
      lst = append(lst, get(other, i))
      i += 1
   }
   lst
}

fn to_str(v){
   "Function `to_str`."
   if(v == true){ return "true" }
   if(v == false){ return "false" }
   if(is_int(v)){ return __to_str(v) }
   if(!v){ return "none" }
   if(is_str(v)){ return v }
   if(!__is_ny_obj(v)){ return __to_str(v) }
   def kind = __tagof(v)
   ; Check for bigint first (it's a list with special marker)
   if(kind == 100 && is_list(v) && len(v) >= 3 && get(v, 0) == 107){ return bigint_to_str(v) }
   return case kind {
      100 -> {
         def n = len(v)
         mut s = "["
         mut i = 0
         while(i < n){
            s = f"{s}{to_str(get(v, i))}"
            if(i < n - 1){ s = f"{s}, " }
            i += 1
         }
         return f"{s}]"
      }
      103 -> {
         def n = len(v)
         mut s = "("
         mut i = 0
         while(i < n){
            s = f"{s}{to_str(get(v, i))}"
            if(i < n - 1){ s = f"{s}, " }
            i += 1
         }
         return f"{s})"
      }
      101 -> "{...}"
      102 -> "{...}"
      122 -> f"<bytes {bytes_len(v)}>"
      110 -> __to_str(v)
      130 -> bigint_to_str(v)
      _   -> f"<ptr {v} tag={__tagof(v)}>"
   }
}

if(comptime{__main()}){
    use std.os.time *
    use std.core.reflect *
    use std.math.float *
    use std.core *

    ; Type
    assert((type(42) == "int"), "type of integer")
    assert((type("hello") == "str"), "type of string")
    assert((type([1, 2, 3]) == "list"), "type of list")
    assert((type(dict(8)) == "dict"), "type of dict")
    assert((type(set()) == "set"), "type of set")
    assert((type(float(1)) == "float"), "type of float")
    assert((type(true) == "bool"), "type of bool")
    assert((type(0) == "int"), "type of zero int")

    ; Len
    assert(len([1, 2, 3]) == 3, "len of list")
    assert(len("hello") == 5, "len of string")
    assert(len([]) == 0, "len of empty list")
    mut d = dict(8)
    d = dict_set(d, "key", "value")
    assert(len(d) == 1, "len of dict")
    assert(len(float(1)) == 0, "len of float")

    ; Contains
    def lst = [1, 2, 3, 4, 5]
    assert(contains(lst, 3), "list contains element")
    assert(!contains(lst, 10), "list doesn't contain element")
    mut s = set()
    s = set_add(s, "a")
    s = set_add(s, "b")
    assert(contains(s, "a"), "set contains element")
    assert(!contains(s, "c"), "set doesn't contain element")
    assert(contains("hello world", "world"), "string contains substring")
    assert(!contains("hello", "xyz"), "string doesn't contain substring")

    ; Eq
    assert((42 == 42), "int equality")
    assert(!(42 == 43), "int inequality")
    assert(("hello" == "hello"), "string equality")
    assert(!("hello" == "world"), "string inequality")
    assert(([1, 2, 3] == [1, 2, 3]), "list equality")
    assert(!([1, 2, 3] == [1, 2, 4]), "list inequality")
    assert(!([1, 2] == [1, 2, 3]), "list different lengths")
    mut d1 = dict(8)
    d1 = dict_set(d1, "a", 1)
    d1 = dict_set(d1, "b", 2)
    mut d2 = dict(8)
    d2 = dict_set(d2, "a", 1)
    d2 = dict_set(d2, "b", 2)
    assert((d1 == d2), "dict equality")
    assert((float(1) == float(1)), "float equality")

    ; Repr
    assert((repr(42) == "42"), "repr of int")
    assert((repr(true) == "true"), "repr of true")
    assert((repr(false) == "false"), "repr of false")
    assert((repr(0) == "0"), "repr of zero int")
    assert((repr("hello") == "\"hello\""), "repr of string")
    assert((repr([1,2,3]) == "[1, 2, 3]"), "repr of list")

    ; Hash
    mut h1 = hash(42)
    mut h2 = hash(42)
    assert(h1 == h2, "hash consistency for int")
    h1 = hash("hello")
    h2 = hash("hello")
    assert(h1 == h2, "hash consistency for string")
    def h3 = hash("world")
    assert(h1 != h3, "different strings have different hashes")

    print("✓ std.core.reflect tests passed")
}
