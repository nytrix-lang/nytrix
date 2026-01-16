;;; mod.ny --- core mod module

;; Keywords: core mod

;;; Commentary:

;; Core Mod module.

use std.core.reflect
use std.strings.str
module std.core (
	bool, load8, load64, store8, store64, load16, load32, store16, store32, ptr_add, ptr_sub,
	malloc, free, realloc, list, is_ptr, is_int, is_num, is_nytrix_obj, is_list, is_dict,
	is_set, is_tuple, is_str, is_bytes, is_float, to_int, from_int, is_kwargs, kwarg,
	is_kwarg, get_kwarg_key, get_kwarg_val, list_len, list_clone, load_item, store_item, get,
	slice, set_idx, append, pop, list_clear, extend, to_str, _to_string, itoa
)

;;; Memory Operations

fn bool(x){
  "Convert a value to its boolean representation."
  return !!x
}

fn load8(p, i=0){
  "Load a single byte from address `p + i`."
  if(i == 0){ return rt_load8(p) }
  return rt_load8_idx(p, i)
}

fn load64(p, i=0){
  "Load an 8-byte integer (uint64) from address `p + i`."
  if(i == 0){ return rt_load64(p) }
  return rt_load64_idx(p, i)
}

fn store8(p, v, i=0){
  "Store byte `v` at address `p + i`."
  if(i == 0){ return rt_store8(p, v) }
  return rt_store8_idx(p, i, v)
}

fn store64(p, v, i=0){
  "Store an 8-byte integer (uint64) to address `p + i`."
  if(i == 0){ return rt_store64(p, v) }
  return rt_store64_idx(p, i, v)
}

fn load16(p, i=0){
  "Load a 2-byte integer (uint16) from address `p + i` using little-endian order."
  if(i == 0){ return rt_load16(p) }
  return rt_load16_idx(p, i)
}

fn load32(p, i=0){
  "Load a 4-byte integer (uint32) from address `p + i` using little-endian order."
  if(i == 0){ return rt_load32(p) }
  return rt_load32_idx(p, i)
}

fn store16(p, v, i=0){
  "Store a 2-byte integer (uint16) at address `p + i` using little-endian order."
  if(i == 0){ return rt_store16(p, v) }
  return rt_store16_idx(p, i, v)
}

fn store32(p, v, i=0){
  "Store a 4-byte integer (uint32) at address `p + i` using little-endian order."
  if(i == 0){ return rt_store32(p, v) }
  return rt_store32_idx(p, i, v)
}

fn ptr_add(p, n){
  "Add an integer offset `n` to a pointer `p`, correctly handling tagged integers."
  return rt_ptr_add(p, n)
}

fn ptr_sub(p, n){
  "Subtract an integer offset `n` from pointer `p`."
  return rt_ptr_sub(p, n)
}

fn malloc(n){
  "Allocates `n` bytes of memory on the heap. Returns a pointer to the allocated memory."
  return rt_malloc(n)
}

fn free(p){
  "Frees memory previously allocated with malloc or realloc."
  return rt_free(p)
}

fn realloc(p, newsz){
  "Resizes the memory block pointed to by `p` to `newsz` bytes."
  return rt_realloc(p, newsz)
}

;;; List Operations

;; List header: [TAG(8) | LEN(8) | CAP(8) | ...items]

fn list(cap=8){
  def p = rt_malloc(16 + cap * 8)
  if(!p){ panic("list malloc failed") }
  store64(p, 100, -8) ; "L" tag
  store64(p, 0, 0)   ; Len
  store64(p, cap, 8) ; Cap
  p
}

fn is_ptr(x){
	"Return true if ptr."
  return rt_is_ptr(x)
}

fn is_int(x){
	"Return true if int."
  return rt_is_int(x)
}

fn is_num(x){
  "Check if a value is a number (integer)."
  return is_int(x)
}

fn is_nytrix_obj(x){
  "Check if a value is a valid Nytrix object pointer (aligned)."
  return rt_is_ptr(x)
}

fn is_list(x){
  "Check if a value is a list."
  if(is_int(x)){ return false }
  if(!is_nytrix_obj(x)){ return false }
  return rt_load64_idx(x, -8) == 100
}

fn is_dict(x){
  "Check if a value is a dictionary."
  if(is_int(x)){ return false }
  if(!is_nytrix_obj(x)){ return false }
  return rt_load64_idx(x, -8) == 101
}

fn is_set(x){
  "Check if a value is a set."
  if(is_int(x)){ return false }
  if(!is_nytrix_obj(x)){ return false }
  return rt_load64_idx(x, -8) == 102
}

fn is_tuple(x){
  "Check if a value is a tuple."
  if(is_int(x)){ return false }
  if(!is_nytrix_obj(x)){ return false }
  def tag = rt_load64_idx(x, -8)
  return tag == 103
}

; define STR_TAG = 120
; define STR_CONST_TAG = 121
; define BYTES_TAG = 122
; define FLOAT_TAG = 110

fn is_str(x){
  "Check if a value is a string."
  if(!is_ptr(x)){ return false }
  def tag = load64(x, -8)
  return tag == 120 || tag == 121 || tag == 241 || tag == 243
}

fn is_bytes(x){
  "Check if a value is a bytes buffer."
  if(!is_ptr(x)){ return false }
  return load64(x, -8) == 122
}

fn is_float(x){
  "Check if a value is a float."
  if(!is_ptr(x)){ return false }
  def tag = load64(x, -8)
  return tag == 110 || tag == 221
}

fn to_int(v){
  "Unwrap a tagged integer to a raw integer."
  return rt_to_int(v)
}

fn from_int(v){
  "Wrap a raw integer into a tagged integer."
  return rt_from_int(v)
}

fn is_kwargs(x){
  "Check if a value is a keyword argument wrapper."
  if(!is_nytrix_obj(x)){ return false }
  return load64(x, -8) == 104
}

fn kwarg(k, v){
  "Create a keyword-argument wrapper object."
  def p = rt_malloc(16) ; Tag at -8, Key(0), Val(8)
  store64(p, 104, -8)   ; Tag 104 for Kwarg at -8
  store64(p, k, 0) ; Key at 0
  store64(p, v, 8) ; Val at 8
  return p
}

fn is_kwarg(x){
	"Return true if kwarg."
  if(!is_ptr(x)){ return false }
  return load64(x, -8) == 104
}

fn get_kwarg_key(x){
	"Return key from a keyword-argument wrapper."
  return rt_load64_idx(x, 0)
}

fn get_kwarg_val(x){
	"Return value from a keyword-argument wrapper."
  return rt_load64_idx(x, 8)
}

fn list_len(lst){
  if(!is_ptr(lst)){ 0 }
  else {
	case rt_load64_idx(lst, -8) {
	  100, 101, 102, 103 -> rt_load64_idx(lst, 0)
	  _ -> 0
	}
  }
}

fn list_clone(lst){
  "Shallow-copies a list, preserving element order."
  if(lst == 0){ return 0 }
  if(is_list(lst) == false){ return 0 }
  def n = list_len(lst)
  def out = list(n)
  def i = 0
  while(i < n){
	  def val = get(lst, i)
	  out = append(out, val)
	  i = i + 1
  }
  return out
}

fn load_item(lst, i){
  "Internal: Loads the i-th item from a collection's raw memory."
  def offset = 16 + i * 8
  return rt_load64_idx(lst, offset)
}

fn store_item(lst, i, v){
  "Internal: Stores value `v` at the i-th position in a collection's raw memory."
  def offset = 16 + i * 8
  rt_store64_idx(lst, offset, v)
  return v
}

fn col_get(obj, i){
  case type(obj) {
	  "str" -> {
		  def n = str_len(obj)
		  if(i < 0){ i = i + n }
		  if(i < 0 || i >= n){ 0 }
		  else {
			use std.strings.str
			str_slice(obj, i, i + 1)
		  }
	  }
	  "dict" -> getitem(obj, i)
	  "list", "tuple" -> {
		  def n = list_len(obj)
		  if(i < 0){ i = i + n }
		  if(i < 0 || i >= n){ 0 }
		  else { load_item(obj, i) }
	  }
	  _ -> 0
  }
}
fn get(obj, i){ col_get(obj, i) }

fn slice(obj, start, stop, step=1){
  "Generic slice operation. Supports strings and lists."
  return case type(obj) {
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
	  _ -> 0
  }
}

fn col_set(obj, i, v){
  case type(obj) {
	  "dict" -> setitem(obj, i, v)
	  "list" -> {
		  def n = list_len(obj)
		  if(i < 0){ i = i + n }
		  if(i < 0 || i >= n){ 0 }
		  else { store_item(obj, i, v) }
	  }
	  _ -> 0
  }
}
fn set_idx(obj, i, v){ col_set(obj, i, v) }
fn list_set(obj, i, v){ col_set(obj, i, v) }

fn list_push(lst, v){
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
	  while(i < n){ store_item(newp, i, load_item(lst, i))  i = i + 1 }
	  free(lst)
	  lst = newp
	}
	store_item(lst, n, v)
	store64(lst, n + 1, 0)
	lst
  }
}
fn append(lst, v){ return list_push(lst, v) }

fn list_pop(lst){
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
fn pop(lst){ list_pop(lst) }

fn list_clear(lst){
  "Removes all elements from the list `lst`."
  if(is_ptr(lst)){
	  store64(lst, 0, 0) ; Store tagged 0 for length at 0
  }
  return lst
}

fn extend(lst, other){
  "Extends list `lst` by appending all elements from list `other`."
  if(is_list(lst) == false){ return lst }
  if(is_list(other) == false){ return lst }
  def i = 0
  def n = list_len(other)
  while(i < n){
	  lst = append(lst, get(other, i))
	  i = i + 1
  }
  return lst
}

;; Low-level IO moved to std.io.mod

fn to_str(v){
  "Convert any value to its string representation."
  return _to_string(v)
}

fn _to_string(v){
	"Internal: convert value to string without full repr for dicts."
  if(is_list(v)){
	  def n = list_len(v)
	  def s = "["
	  def i = 0
	  while(i < n){
		  s = f"{s}{_to_string(get(v, i))}"
		  if(i < n - 1){ s = f"{s}, " }
		  i = i + 1
	  }
	  return f"{s}]"
  }
  if(is_dict(v)){ return "{...}" }
  if(is_bytes(v)){ return f"<bytes {bytes_len(v)}>" }
  return rt_to_str(v)
}
fn itoa(v){
  "Alias for to_str."
  return to_str(v)
}
