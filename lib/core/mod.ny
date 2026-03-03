;; Keywords: core
;; Core module.

module std.core (
   bool, init_str, load8, load16, load32, load64, load32_f32, load64_f64,
   store8, store16, store32, store64, store32_f32, store64_f64,
   memcpy, memset, memcmp, memchr,
   ptr_add, ptr_sub,
   malloc, free, realloc, zalloc, list, is_ptr, is_int, is_nytrix_obj, is_list, is_dict,
   is_set, is_tuple, is_str, is_bytes, is_float, to_int, from_int, is_kwargs, __kwarg, kwarg,
   get_kwarg_key, get_kwarg_val, len, list_clone, load_item, store_item, get,
   slice, set_idx, append, pop, list_clear, extend, sort, to_str, typeof,
   dict, dict_len, dict_get, dict_has, dict_set, dict_del, dict_clone, dict_merge,
   dict_items, dict_keys, dict_values,
   set, set_add, set_contains,
   add, sub, mul, div, mod, band, bor, bxor, bshl, bshr, bnot,
   eq, lt, le, gt, ge, argc, argv, __argv, envc, envp, errno,
   globals, set_globals,
   OS, ARCH, IS_LINUX, IS_MACOS, IS_WINDOWS, IS_X86_64, IS_AARCH64, IS_ARM,
   is_truthy, is_falsy, not_none, panic_if, print
)
use std.core.primitives *
use std.core.reflect as core_ref
use std.core.dict_mod *
use std.core.set_mod *
use std.core.mem as core_mem

fn is_truthy(x){
  "Returns **true** if value `x` is considered 'truthy'.
   - `none`: false
   - `int`: true if not 0
   - `str/list/dict/set/tuple`: true if length > 0
   - `other`: true"
  if(!x){ return false }
  if(__is_int(x)){ return x != 0 }
  if(is_str(x) || is_list(x) || is_dict(x) || is_set(x) || is_tuple(x)){
     return len(x) > 0
  }
  true
}

fn is_falsy(x){
  "Returns **true** if value `x` is 'falsy' (not truthy)."
  !is_truthy(x)
}

fn not_none(x, fallback){
  "Returns `x` if it is not **none**; otherwise returns `fallback`."
  if(x){ return x } fallback
}

fn panic_if(cond, msg){
  "Panics with `msg` if `cond` is truthy."
  if(is_truthy(cond)){ panic(msg) }
}

fn _print_raw(s){
   "Internal: writes raw string content to stdout."
   if(!is_str(s)){ return 0 }
   def n = load64(s, -16)
   if(n > 0){ __sys_write_off(1, s, n, 0) }
   0
}

fn print(...args){
  "Prints one or more values to stdout, separated by spaces and ending with a newline."
  mut i = 0
  def n = len(args)
  while(i < n){
     _print_raw(to_str(get(args, i)))
     if(i < n - 1){ _print_raw(" ") }
     i += 1
  }
  _print_raw("\n")
  0
}

fn bool(x){
  "Converts `x` to a primitive boolean value."
  !!x
}

fn init_str(p, n){
  "Initializes a raw memory block `p` of size `n` as a Nytrix string object."
  store8(p, 120, -8)
  store64(p, n, -16)
  p
}

fn load8(p, i=0){
  "Loads a single byte from address `p + i`."
  __load8_idx(p, i)
}
fn load64(p, i=0){
  "Loads a 64-bit integer from address `p + i`."
  __load64_idx(p, i)
}
fn store8(p, v, i=0){
  "Stores byte `v` at address `p + i`."
  __store8_idx(p, i, v)
}
fn store64(p, v, i=0){
  "Stores 64-bit integer `v` at address `p + i`."
  __store64_idx(p, i, v)
}

fn store64_raw(p, v, i=0){
  "Stores a raw integer `v` at address `p + i` as two 32-bit parts."
  def raw = to_int(v)
  store32(p, band(raw, 0xFFFFFFFF), i)
  store32(p, bshr(raw, 32), i + 4)
}

fn load16(p, i=0){
  "Loads a 16-bit integer from address `p + i`."
  __load16_idx(p, i)
}
fn load32(p, i=0){
  "Loads a 32-bit integer from address `p + i`."
  __load32_idx(p, i)
}
fn store16(p, v, i=0){
  "Stores a 16-bit integer `v` at address `p + i`."
  __store16_idx(p, i, v)
}
fn store32(p, v, i=0){
  "Stores a 32-bit integer `v` at address `p + i`."
  __store32_idx(p, i, v)
}

fn load32_f32(p, i=0){
  "Loads a 32-bit float from address `p + i` and boxes it."
  __flt_box_val32(__load32_idx(p, i))
}
fn store32_f32(p, v, i=0){
  "Unboxes float `v` and stores it as a 32-bit float at address `p + i`."
  __store32_idx(p, i, __flt_unbox_val32(v))
}

fn load64_f64(p, i=0){
  "Loads a 64-bit float (double) from address `p + i` and boxes it."
  __flt_box_val(__load64_idx(p, i))
}
fn store64_f64(p, v, i=0){
  "Unboxes float `v` and stores it as a 64-bit float (double) at address `p + i`."
  __store64_idx(p, i, __flt_unbox_val(v))
}

fn memcpy(dst, src, n){
  "Copies `n` bytes from `src` to `dst`."
  __memcpy(dst, src, n)
}
fn memset(dst, v, n){
  "Sets `n` bytes at `dst` to value `v`."
  __memset(dst, v, n)
}
fn memcmp(a, b, n){
  "Compares `n` bytes of memory blocks `a` and `b`."
  __memcmp(a, b, n)
}
fn memchr(p, c, n){
  "Searches for byte `c` in the first `n` bytes of `p`."
  core_mem.memchr(p, c, n)
}

fn ptr_add(p, n){
  "Returns address `p + n`."
  __add(p, n)
}
fn ptr_sub(p, n){
  "Returns address `p - n`."
  __sub(p, n)
}

fn malloc(n){
  "Allocates `n` bytes of heap memory."
  __malloc(n)
}
fn free(p){
  "Frees heap memory block `p`."
  __free(p)
}
fn realloc(p, n){
  "Resizes heap memory block `p` to `n` bytes."
  __realloc(p, n)
}
fn zalloc(n){
  "Allocates `n` bytes of heap memory and zeroes it out."
  def p = malloc(n)
  if(p){ memset(p, 0, n) }
  p
}

fn list(cap=8){
  "Creates a new empty list with initial capacity `cap`."
  def size = 16 + cap * 8
  def p = malloc(size)
  memset(p, 0, size)
  store8(p, 100, -8)
  store64(p, 0, 0)
  store64(p, cap, 8)
  p
}

fn is_nytrix_obj(x){
  "Returns **true** if `x` is a Nytrix-managed heap object."
  def tag = __tagof(x)
  if(!__is_int(tag)){ return false }
  tag >= 100 && tag <= 255
}

fn is_list(x){
  "Returns **true** if `x` is a list."
  __tagof(x) == 100
}
fn is_dict(x){
  "Returns **true** if `x` is a dictionary."
  __tagof(x) == 101
}
fn is_set(x){
  "Returns **true** if `x` is a set."
  __tagof(x) == 102
}
fn is_tuple(x){
  "Returns **true** if `x` is a tuple."
  __tagof(x) == 103
}
fn is_str(x){
  "Returns **true** if `x` is a string."
  if(!x || __is_int(x)){ return false }
  def t = __tagof(x)
  t == 120 || t == 121
}
fn is_bytes(x){
  "Returns **true** if `x` is a bytes object."
  __tagof(x) == 122
}
fn is_float(x){
  "Returns **true** if `x` is a float object."
  __tagof(x) == 110
}

fn to_int(v){
  "Untags a Nytrix value into a raw integer."
  __untag(v)
}
fn from_int(v){
  "Tags a raw integer `v` into a Nytrix value."
  __tag(v)
}

fn __kwarg(k, v){
  "Internal: creates a keyword argument object wrap."
  def p = malloc(16)
  store64(p, k, 0)
  store64(p, v, 8)
  store8(p, 150, -8)
  p
}

fn is_kwargs(x){
  "Returns **true** if `x` is a keyword argument wrap object."
  if(!x || __is_int(x)){ return false }
  __tagof(x) == 150
}

fn kwarg(k, v){
  "Creates a keyword argument pair for function calls."
  __kwarg(k, v)
}
fn get_kwarg_key(x){
  "Returns the key from keyword argument wrap `x`."
  load64(x, 0)
}
fn get_kwarg_val(x){
  "Returns the value from keyword argument wrap `x`."
  load64(x, 8)
}

fn get(obj, key, default=0){
  "Generic element retriever for strings, lists, dicts, and tuples."
  core_ref.get(obj, key, default)
}
fn len(x){
  "Returns the number of elements in a collection or the length of a string."
  core_ref.len(x)
}

fn list_clone(lst){
  "Returns a shallow clone of list `lst`."
  if(!is_list(lst)){ return 0 }
  def n = len(lst)
  mut out = list(n)
  mut i = 0
  while(i < n){
     append(out, get(lst, i))
     i += 1
  }
  out
}

fn load_item(lst, i){
  "Loads the item at index `i` from list `lst`."
  load64(lst, 16 + i * 8)
}
fn store_item(lst, i, v){
  "Stores value `v` at index `i` in list `lst`."
  store64(lst, v, 16 + i * 8) v
}

fn sort(lst){
  "Sorts list `lst` in-place using insertion sort."
  if(!is_list(lst)){ return lst }
  def n = len(lst)
  mut i = 1
  while(i < n){
     def key = load64(lst, 16 + i * 8)
     mut j = i - 1
     while(j >= 0 && load64(lst, 16 + j * 8) > key){
        store64(lst, load64(lst, 16 + j * 8), 16 + (j + 1) * 8)
        j -= 1
     }
     store64(lst, key, 16 + (j + 1) * 8)
     i += 1
  }
  lst
}

fn list_clear(lst){
  "Removes all elements from list `lst`."
  if(!is_list(lst)){ return lst }
  store64(lst, 0, 0)
  lst
}

fn add(a, b){
  "Generic addition for primitives and objects (handles bigint)."
  core_ref.add(a, b)
}
fn sub(a, b){
  "Generic subtraction for primitives and objects (handles bigint)."
  core_ref.sub(a, b)
}
fn mul(a, b){
  "Generic multiplication for primitives and objects (handles bigint)."
  core_ref.mul(a, b)
}
fn div(a, b){
  "Generic division for primitives and objects (handles bigint)."
  core_ref.div(a, b)
}

def OS = __os_name()
def ARCH = __arch_name()
def IS_LINUX   = __eq(OS, "linux")
def IS_MACOS   = __eq(OS, "macos")
def IS_WINDOWS = __eq(OS, "windows")
def IS_X86_64  = __eq(ARCH, "x86_64")
def IS_AARCH64 = __eq(ARCH, "aarch64") || __eq(ARCH, "arm64")
def IS_ARM     = __eq(ARCH, "arm")
