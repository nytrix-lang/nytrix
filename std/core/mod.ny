;; Keywords: core mod
;; Core Mod module.

module std.core (
   bool, load8, load64, store8, store64, load16, load32, store16, store32, ptr_add, ptr_sub,
   malloc, free, realloc, list, is_ptr, is_int, is_nytrix_obj, is_list, is_dict,
   is_set, is_tuple, is_str, is_bytes, is_float, to_int, from_int, is_kwargs, kwarg,
   get_kwarg_key, get_kwarg_val, list_len, list_clone, load_item, store_item, get,
   slice, set_idx, append, pop, list_clear, extend, to_str
)

;;; Memory Operations

fn bool(x){
  "Convert any value to its **boolean** representation. Returns `1` (true) if the value is truthy, `0` (false) otherwise."
  !!x
}

fn __init_str(p, n){
  "Internal: Initialize string header at pointer `p` with length `n`."
  store64(p, to_int(241), -8) ; Raw TAG_STR
  store64(p, n, -16)          ; Tagged Length
  p
}

fn load8(p, i=0){
  "Load a single byte from address `p + i`."
  __load8_idx(p, i)
}

fn load64(p, i=0){
  "Load an 8-byte integer (**uint64**) from address `p + i`."
  __load64_idx(p, i)
}

fn store8(p, v, i=0){
  "Store byte `v` at address `p + i`."
  __store8_idx(p, i, v)
}

fn store64(p, v, i=0){
  "Store an 8-byte integer (**uint64**) to address `p + i`."
  __store64_idx(p, i, v)
}

fn load16(p, i=0){
  "Load a 2-byte integer (**uint16**) from address `p + i` using little-endian order."
  __load16_idx(p, i)
}

fn load32(p, i=0){
  "Load a 4-byte integer (**uint32**) from address `p + i` using little-endian order."
  __load32_idx(p, i)
}

fn store16(p, v, i=0){
  "Store a 2-byte integer (**uint16**) at address `p + i` using little-endian order."
  __store16_idx(p, i, v)
}

fn store32(p, v, i=0){
  "Store a 4-byte integer (**uint32**) at address `p + i` using little-endian order."
  __store32_idx(p, i, v)
}

fn ptr_add(p, n){
  "Add an integer offset `n` to a pointer `p`. Correct-handling of tagged integers by the runtime."
  p + n
}

fn ptr_sub(p, n){
  "Subtract an integer offset `n` from pointer `p`."
  p - n
}

fn malloc(n){
  "Allocates `n` bytes of memory on the heap. Returns a `ptr` to the allocated memory."
  __malloc(n)
}

fn free(p){
  "Frees memory previously allocated with [[std.core::malloc]]."
  __free(p)
}

fn realloc(p, newsz){
  "Resizes the memory block pointed to by `p` to `newsz` bytes."
  __realloc(p, newsz)
}

;;; List Operations

;; List header: [TAG(8) | LEN(8) | CAP(8) | ...items]

fn list(cap=8){
  "Creates a new empty list with the given initial capacity **cap** (default 8)."
  def p = __malloc(16 + cap * 8)
  if(!p){ panic("list malloc failed") }
  store64(p, 100, -8) ; "L" tag
  store64(p, 0, 0)    ; Len
  store64(p, cap, 8)  ; Cap
  p
}

fn is_ptr(x){
  "Returns **true** if `x` is a pointer (heap address)."
  __is_ptr(x)
}

fn is_int(x){
  "Returns **true** if `x` is a tagged integer."
  __is_int(x)
}

fn is_nytrix_obj(x){
  "Check if a value is a valid Nytrix object pointer (aligned)."
  is_ptr(x)
}

fn is_list(x){
  "Returns **true** if the value `x` is a **list**."
  if(is_int(x)){ return false }
  if(!is_nytrix_obj(x)){ return false }
  __load64_idx(x, -8) == 100
}

fn is_dict(x){
  "Returns **true** if the value `x` is a **dictionary**."
  if(is_int(x)){ return false }
  if(!is_nytrix_obj(x)){ return false }
  __load64_idx(x, -8) == 101
}

fn is_set(x){
  "Returns **true** if the value `x` is a **set**."
  if(is_int(x)){ return false }
  if(!is_nytrix_obj(x)){ return false }
  __load64_idx(x, -8) == 102
}

fn is_tuple(x){
  "Returns **true** if the value `x` is a **tuple**."
  if(is_int(x)){ return false }
  if(!is_nytrix_obj(x)){ return false }
  def tag = __load64_idx(x, -8)
  tag == 103
}

fn is_str(x){
  "Returns **true** if the value `x` is a **string**."
  if(!is_ptr(x)){ return false }
  def tag = load64(x, -8)
  tag == 120 || tag == 121 || tag == 241 || tag == 243
}

fn is_bytes(x){
  "Returns **true** if the value `x` is a **bytes buffer**."
  if(!is_ptr(x)){ return false }
  load64(x, -8) == 122
}

fn is_float(x){
  "Returns **true** if the value `x` is a **floating-point** number."
  if(!is_ptr(x)){ return false }
  def tag = load64(x, -8)
  tag == 110 || tag == 221
}

fn to_int(v){
  "Unwraps a tagged integer or pointer value to a raw, untagged integer."
  asm("sarq $$1, $0", "=r,0", v)
}

fn from_int(v){
  "Wraps a raw machine integer into a Nytrix-tagged integer."
  asm("leaq 1(,$0,2), $0", "=r,0", v)
}

fn is_kwargs(x){
  "Internal: Returns **true** if `x` is a keyword-argument wrapper object."
  if(!is_nytrix_obj(x)){ return false }
  load64(x, -8) == 104
}

fn kwarg(k, v){
  "Create a keyword-argument wrapper object combining key `k` and value `v`."
  def p = __malloc(16) ; Tag at -8, Key(0), Val(8)
  store64(p, 104, -8)   ; Tag 104 for Kwarg at -8
  store64(p, k, 0) ; Key at 0
  store64(p, v, 8) ; Val at 8
  p
}

fn get_kwarg_key(x){
  "Extracts the key from a [[std.core::kwarg]] object."
  __load64_idx(x, 0)
}

fn get_kwarg_val(x){
  "Extracts the value from a [[std.core::kwarg]] object."
  __load64_idx(x, 8)
}

fn list_len(lst){
  "Returns the number of elements in a list, dictionary, set, or tuple. Returns `0` for other types."
  if(!is_ptr(lst)){ 0 }
  else {
   case __load64_idx(lst, -8) {
     100, 101, 102, 103 -> __load64_idx(lst, 0)
     _ -> 0
   }
  }
}

fn list_clone(lst){
  "Creates a **shallow copy** of the list `lst`."
  if(lst == 0){ return 0 }
  if(is_list(lst) == false){ return 0 }
  def n = list_len(lst)
  def out = list(n)
  def i = 0
  while(i < n){
     def val = get(lst, i)
     out = append(out, val)
     i += 1
  }
  out
}

fn load_item(lst, i){
  "Internal: Loads the value at index `i` from the raw data section of a collection."
  def offset = 16 + i * 8
  __load64_idx(lst, offset)
}

fn store_item(lst, i, v){
  "Internal: Stores value `v` at index `i` in the raw data section of a collection."
  def offset = 16 + i * 8
  __store64_idx(lst, offset, v)
  v
}

fn get(obj, i){
  "Generic element retriever. Handles indexing for strings, lists, dicts, and tuples."
  case type(obj) {
     "str" -> {
        def n = str_len(obj)
        if(i < 0){ i += n }
        if(i < 0 || i >= n){ 0 }
        else {
         use std.strings.str
         str_slice(obj, i, i + 1)
        }
     }
     "dict" -> dict_get(obj, i)
     "list", "tuple" -> {
        def n = list_len(obj)
        if(i < 0){ i += n }
        if(i < 0 || i >= n){ 0 }
        else { load_item(obj, i) }
     }
     _ -> 0
  }
}

fn slice(obj, start, stop, step=1){
  "Generic **slice** operation for strings and lists."
  case type(obj) {
     "str" -> {
      use std.strings.str
      str_slice(obj, start, stop, step)
     }
     "list" -> {
        def n = list_len(obj)
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
        def out = list(8)
        def i = start
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
     _ -> 0
  }
}

fn set_idx(obj, i, v){
  "Generic element setter. Supported for dicts and lists."
  case type(obj) {
     "dict" -> dict_set(obj, i, v)
     "list" -> {
        def n = list_len(obj)
        if(i < 0){ i += n }
        if(i < 0 || i >= n){ 0 }
        else { store_item(obj, i, v) }
     }
     _ -> 0
  }
}

fn append(lst, v){
  "Appends value `v` to the end of list `lst`. Returns the (possibly reallocated) list ptr."
  if(!is_nytrix_obj(lst)){ lst }
  else {
      def tag = load64(lst, -8)
   def n = load64(lst, 0)
   def cap = load64(lst, 8)
   if(n >= cap){
     def newcap = case cap { 0 -> 8 _ -> cap * 2 }
     def newp = list(newcap)
     store64(newp, tag, -8)
     def i = 0
     while(i < n){ store_item(newp, i, load_item(lst, i))  i += 1 }
     free(lst)
     lst = newp
   }
   store_item(lst, n, v)
   store64(lst, n + 1, 0)
   lst
  }
}

fn pop(lst){
  "Removes and returns the last element from list `lst`. Returns `0` if empty."
  if(!is_ptr(lst)){ 0 }
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

fn list_clear(lst){
  "Removes all elements from the list `lst` by resetting its length to zero."
  if(is_ptr(lst)){
     store64(lst, 0, 0) ; Store tagged 0 for length at 0
  }
  lst
}

fn extend(lst, other){
  "Appends all elements from collection `other` to the list `lst`."
  if(is_list(lst) == false){ return lst }
  if(is_list(other) == false){ return lst }
  def i = 0
  def n = list_len(other)
  while(i < n){
     lst = append(lst, get(other, i))
     i += 1
  }
  lst
}

fn to_str(v){
  "Converts any Nytrix value to its string representation. Handles recursive collection printing."
  if(is_list(v)){
     def n = list_len(v)
     def s = "["
     def i = 0
     while(i < n){
        s = f"{s}{to_str(get(v, i))}"
        if(i < n - 1){ s = f"{s}, " }
        i += 1
     }
     f"{s}]"
  } else {
    if(is_dict(v)){ "{...}" }
    elif(is_bytes(v)){ f"<bytes {bytes_len(v)}>" }
    else { __to_str(v) }
  }
}