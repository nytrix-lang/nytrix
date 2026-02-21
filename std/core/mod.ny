;; Keywords: core
;; Core module.

module std.core (
   bool, init_str, load8, load64, store8, store64, load16, load32, store16, store32, ptr_add, ptr_sub,
   malloc, free, realloc, zalloc, list, is_ptr, is_int, is_nytrix_obj, is_list, is_dict,
   is_set, is_tuple, is_str, is_bytes, is_float, to_int, from_int, is_kwargs, __kwarg, kwarg,
   get_kwarg_key, get_kwarg_val, len, list_clone, load_item, store_item, get,
   slice, set_idx, append, pop, list_clear, extend, sort, to_str, typeof,
   set, set_add, set_contains,
   add, sub, mul, div, mod, band, bor, bxor, bshl, bshr, bnot,
   eq, lt, le, gt, ge, argc, argv, __argv, envc, envp, errno,
   globals, set_globals,
   OS, ARCH, IS_LINUX, IS_MACOS, IS_WINDOWS, IS_X86_64, IS_AARCH64, IS_ARM
)
use std.core.primitives *
use std.core.reflect as core_ref
use std.core.set as core_set

;;; Memory Operations

fn bool(x){
  "Convert any value to its **boolean** representation. Returns `1` (true) if the value is truthy, `0` (false) otherwise."
  !!x
}

fn init_str(p, n){
  "Initialize string header at pointer `p` with length `n`."
  store64(p, 120, -8) ; Raw TAG_STR
  store64(p, n, -16) ; Tagged Length
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
  "Allocates `n` bytes on the heap. Returns a ptr."
  __malloc(n)
}

fn free(p){
  "Frees memory previously allocated with [[std.core::malloc]]."
  if(!p){ return 0 }
  __free(p)
}

fn realloc(p, newsz){
  "Resizes the memory block pointed to by `p` to `newsz` bytes."
  if(!p){ return malloc(newsz) }
  __realloc(p, newsz)
}

fn zalloc(n){
  "Allocates `n` bytes on the heap and zero-initializes them. Currently aliases malloc."
  malloc(n)
}

;;; List Operations

;; List header: [TAG(8) | LEN(8) | CAP(8) | ...items]

fn list(cap=8){
  "Creates a new empty list with the given initial capacity **cap** (default 8)."
  def p = malloc(16 + cap * 8)
  if(!p){ panic("list malloc failed") }
  store64(p, 100, -8) ; "L" tag
  store64(p, 0, 0) ; Len
  store64(p, cap, 8) ; Cap
  p
}

fn set(cap=8){
  "Creates a new empty set."
  core_set.set(cap)
}

fn set_add(s, key){
  "Adds `key` to set `s`. Returns the (possibly reallocated) set."
  core_set.set_add(s, key)
}

fn set_contains(s, key){
  "Returns true if `key` is in set `s`."
  core_set.set_contains(s, key)
}

fn eq(a, b){
  "Structural equality (delegates to std.core.reflect.eq)."
  core_ref.eq(a, b)
}

fn add(a, b){
  "Generic addition (delegates to std.core.reflect.add)."
  core_ref.add(a, b)
}

fn sub(a, b){
  "Generic subtraction (delegates to std.core.reflect.sub)."
  core_ref.sub(a, b)
}

fn mul(a, b){
  "Generic multiplication (delegates to std.core.reflect.mul)."
  core_ref.mul(a, b)
}

fn div(a, b){
  "Generic division (delegates to std.core.reflect.div)."
  core_ref.div(a, b)
}

fn is_nytrix_obj(x){
  "Check if a value is a valid Nytrix object pointer (aligned)."
  __is_ny_obj(x)
}

fn is_list(x){
  "Returns **true** if the value `x` is a **list**."
  if(!__is_ny_obj(x)){ return false }
  return __tagof(x) == 100
}

fn is_dict(x){
  "Returns **true** if the value `x` is a **dictionary**."
  if(!__is_ny_obj(x)){ return false }
  return __tagof(x) == 101
}

fn is_set(x){
  "Returns **true** if the value `x` is a **set**."
  if(!__is_ny_obj(x)){ return false }
  return __tagof(x) == 102
}

fn is_tuple(x){
  "Returns **true** if the value `x` is a **tuple**."
  if(!__is_ny_obj(x)){ return false }
  return __tagof(x) == 103
}

fn is_str(x){
  "Returns **true** if the value `x` is a **string**."
  __is_str_obj(x)
}

fn is_bytes(x){
  "Returns **true** if the value `x` is a **bytes buffer**."
  if(!__is_ny_obj(x)){ return false }
  __tagof(x) == 122
}

fn is_float(x){
  "Returns **true** if the value `x` if a **floating-point** number."
  __is_float_obj(x)
}

fn to_int(v){
  "Unwraps a tagged integer or pointer value to a raw, untagged integer."
  __untag(v)
}

fn from_int(v){
  "Wraps a raw machine integer into a Nytrix-tagged integer."
  __tag(v)
}

fn argc(){
  "Returns the process argument count."
  __argc()
}
fn argv(){
  "Returns the raw argv pointer."
  __argvp()
}
fn envc(){
  "Returns the number of environment entries."
  __envc()
}
fn envp(){
  "Returns the raw envp pointer."
  __envp()
}
fn errno(){
  "Returns the current C runtime errno value."
  __errno()
}

fn is_kwargs(x){
  "Internal: Returns **true** if `x` is a keyword-argument wrapper object."
  if(!__is_ny_obj(x)){ return false }
  __tagof(x) == 104
}

fn __kwarg(k, v){
  "Create a keyword-argument wrapper object combining key `k` and value `v`."
  def p = malloc(16) ; Tag at -8, Key(0), Val(8)
  store64(p, 104, -8) ; Tag 104 for Kwarg (stored as 209)
  store64(p, k, 0) ; Key at 0
  store64(p, v, 8) ; Val at 8
  p
}

fn kwarg(k, v){
  "Create a keyword-argument wrapper object."
  __kwarg(k, v)
}

fn get_kwarg_key(x){
  "Extracts the key from a [[std.core::kwarg]] object."
  load64(x, 0)
}

fn get_kwarg_val(x){
  "Extracts the value from a [[std.core::kwarg]] object."
  load64(x, 8)
}

fn len(x){
  "Returns the number of elements in a collection or length of a string/bytes. Returns `0` for other types."
  core_ref.len(x)
}

fn list_clone(lst){
  "Creates a **shallow copy** of the list `lst`."
  if(lst == 0){ return 0 }
  if(!is_list(lst)){ return 0 }
  def n = len(lst)
  mut out = list(n)
  mut i = 0
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
  load64(lst, offset)
}

fn store_item(lst, i, v){
  "Internal: Stores value `v` at index `i` in the raw data section of a collection."
  def offset = 16 + i * 8
  store64(lst, v, offset)
  v
}

fn sort(lst){
  "In-place ascending sort for lists of numbers."
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
  "Removes all elements from the list."
  if(!is_list(lst)){ return lst }
  store64(lst, 0, 0)
  lst
}

def OS = __os_name()
def ARCH = __arch_name()
def IS_LINUX   = __eq(OS, "linux")
def IS_MACOS   = __eq(OS, "macos")
def IS_WINDOWS = __eq(OS, "windows")
def IS_X86_64  = __eq(ARCH, "x86_64")
def IS_AARCH64 = __eq(ARCH, "aarch64") || __eq(ARCH, "arm64")
def IS_ARM     = __eq(ARCH, "arm")

if(comptime{__main()}){
    use std.core *
    use std.core.list *
    use std.os *
    use std.os.dirs *
    use std.os.path *
    use std.str *
    use std.os.sys *

    ; Memory operations
    def ptr = malloc(64)
    assert(ptr != 0, "malloc returns non-null")
    store64(ptr, 12345)
    mut val = load64(ptr)
    assert(val == 12345, "store64/load64")
    store8(ptr, 255)
    val = load8(ptr)
    assert(val == 255, "store8/load8")
    free(ptr)

    ; List operations
    mut lst = list(8)
    assert(is_list(lst), "list creation")
    assert(len(lst) == 0, "empty list length")
    lst = append(lst, 10)
    lst = append(lst, 20)
    lst = append(lst, 30)
    assert(len(lst) == 3, "list length after appends")
    assert(get(lst, 0) == 10, "get first element")
    assert(get(lst, 1) == 20, "get second element")
    assert(get(lst, 2) == 30, "get third element")
    assert(get(lst, -1) == 30, "negative indexing")
    set_idx(lst, 1, 25)
    assert(get(lst, 1) == 25, "set element")
    val = pop(lst)
    assert(val == 30, "pop returns last element")
    assert(len(lst) == 2, "length after pop")
    def lst2 = [40, 50]
    lst = extend(lst, lst2)
    assert(len(lst) == 4, "extend length")
    lst = list_clear(lst)
    assert(len(lst) == 0, "clear list")

    ; Type checking
    assert(is_int(42), "is_int on integer")
    assert(!is_int("string"), "is_int on string")
    assert(is_ptr("string"), "is_ptr on string")
    assert(!is_ptr(42), "is_ptr on integer")
    assert(is_list([1, 2, 3]), "is_list on list")
    assert(!is_list(42), "is_list on integer")
    def d = dict(8)
    assert(is_dict(d), "is_dict on dict")
    assert(!is_dict([]), "is_dict on list")

    ; 'in' operator
    def in_list = [1, 2, 3]
    assert(contains(in_list, 1), "in operator on list")
    assert(!contains(in_list, 4), "in operator on list (not found)")
    def in_str = "hello"
    assert(contains(in_str, "ell"), "in operator on string")
    assert(!contains(in_str, "xyz"), "in operator on string (not found)")

    ; File operations
    def test_file = normalize(temp_dir() + sep() + "nytrix_core_test.txt")
    def test_data = "Hello, Nytrix!"
    mut result = file_remove(test_file) ; Cleanup previous run
    result = file_write(test_file, test_data)

    if(is_err(result)){
        print("file_write failed to: " + test_file)
        print("Error code: " + to_str(__unwrap(result)))
        print("System errno: " + to_str(errno()))
        panic("file_write failed")
    }

    assert(is_ok(result), "file_write returns ok")
    assert(__unwrap(result) > 0, "file_write returns bytes written")
    assert(file_exists(test_file), "file exists after write")
    mut r = file_read(test_file)
    assert(is_ok(r), "file_read returns ok")
    mut content = __unwrap(r)
    assert((content == test_data), "file content matches")

    mut append_res = file_append(test_file, " More data")
    assert(is_ok(append_res), "file_append returns ok")
    r = file_read(test_file)
    assert(is_ok(r), "file_read(2) returns ok")
    content = __unwrap(r)
    assert(str_contains(content, "More data"), "file append works")

    mut remove_res = file_remove(test_file)
    assert(is_ok(remove_res), "file remove")
    assert(!file_exists(test_file), "file removed")

    ; String helpers
    def s1 = "hello"
    def s2 = "hello"
    def s3 = "world"
    assert(_str_eq(s1, s2), "string equality")
    assert(!_str_eq(s1, s3), "string inequality")

    print("✓ std.core.mod tests passed")
}
