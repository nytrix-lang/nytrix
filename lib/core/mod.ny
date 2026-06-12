;; Keywords: core runtime primitives containers list dict set tuple range string bytes result assert reflection queue channel memory
;; Core runtime facade: primitives, containers, strings, assertions, Result values, queues, and channels.
;; References:
;; - std
module std.core(bool, init_str, load8, load16, load32, load64, load32_h, load64_h, load64_i, load32_f32, load64_f64, store8, store16, store32, store64, store32_h, store64_h, store64_i, store32_f32, store64_f64, memcpy, memset, memcmp, memchr, ptr_add, ptr_sub, malloc, free, malloc_raw, free_raw, realloc, zalloc, list, vec2, vec3, vec4, bytes, bytes_get, bytes_set, Vector2, Vector3, Vector4, is_ptr, is_int, is_nytrix_obj, is_list, is_dict, is_set, is_tuple, is_range, is_str, is_bytes, is_float, to_int, from_int, is_kwargs, __kwarg, kwarg, get_kwarg_key, get_kwarg_val, len, clone, load_item, store_item, swap, swapped, get, set_idx, index_read, slice, put, delete, clear, append, pop, extend, sort, sorted, replace, join, to_str, str, dict, dict_has, dict_del, dict_pop, dict_popitem, dict_setdefault, dict_clone, dict_merge, dict_items, dict_keys, dict_values, dict_clear, items, keys, values, set, contains, startswith, endswith, type, type_shape, is_shape, require_shape, assert_shape, hash, repr, debug_print_val, debug_print, breakpoint, print_history_drain, print_history_clear, print_to_stdout, add, sub, mul, div, mod, pow, band, bor, bxor, bshl, bshr, bnot, eq, ne, lt, le, gt, ge, argc, argv, __argv, envc, envp, errno, atoi, globals, set_globals, OS, ARCH, IS_LINUX, IS_MACOS, IS_WINDOWS, IS_X86_64, IS_AARCH64, IS_ARM, is_truthy, is_falsy, not_none, min, max, sqrt, ok, err, is_ok, is_err, unwrap, unwrap_or, panic, panic_if, assert, assert_eq, print, eprint, chr, retain, rc_count, _pow2, __big_add_abs, __big_sub_abs, __big_mul_abs, _clone_list, mapcat, flatten, map, filter, take, drop, reverse, range, range2, reduce, each, count, count_if, first, last, compact, chunk, windowed, Counter, counter, counter_add, counter_inc, counter_update, count_by, most_common, group_by, default_get, Queue, queue, queue_push, queue_pop, queue_try_pop, queue_peek, queue_len, queue_empty, queue_clear, Channel, channel, chan, chan_send, chan_try_send, chan_recv, chan_try_recv, chan_close, chan_closed, chan_len)
use std.core.primitives
use std.core.reflect as core_ref
use std.core.dict_mod
use std.core.set_mod
use std.core.set_mod as core_set
use std.core.mem as core_mem
use std.core.error as error
use std.core.iter
use std.core.iter as core_iter
use std.core.debug
use std.core.collections as collections
use std.core.str as core_str

mut _print_history = nil
mut print_to_stdout = true

fn _ensure_print_history() list {
   if(!_print_history){ _print_history = borrow([]) }
   _print_history
}

fn _append_print_history(any line) int {
   if(!is_str(line)){ line = to_str(line) }
   _ensure_print_history()
   _print_history = borrow(_print_history.append(line))
   if(_print_history.len > 512){ _print_history = borrow(slice(_print_history, _print_history.len - 512, _print_history.len, 1)) }
   0
}

fn print_history_drain() list {
   "Returns the accumulated print history and clears the internal buffer."
   _ensure_print_history()
   def out = _print_history
   _print_history = borrow([])
   out
}

fn print_history_clear() int {
   "Clears the accumulated print history buffer."
   _print_history = borrow([])
   0
}

fn assert(any cond, str msg="assert failed") any {
   "Panics with `msg` if `cond` is false. Useful for core invariants and unit tests."
   if(!cond){ panic(msg) }
}

fn assert_eq(any a, any b, str msg="assert eq failed") any {
   "Panics with `msg` if `a` and `b` are not structurally equal. Uses the built-in `eq` operator."
   if(!eq(a, b)){ panic(msg + ": expected " + to_str(b) + ", got " + to_str(a)) }
}

fn _core_pow_float_arg(any x) any {
   if(type(x) == "bigint"){ return from_int(__bigint_to_int(x)) }
   x
}

fn pow(any a, any b) any {
   "Raises `a` to exponent `b`. Integer powers are exact; float operands and negative exponents use floating-point power."
   def ta = type(a)
   def tb = type(b)
   def a_num = ta == "int" || ta == "float" || ta == "bigint"
   def b_num = tb == "int" || tb == "float" || tb == "bigint"
   if(!a_num || !b_num){ panic("pow expects numeric operands") }
   if(ta == "float" || tb == "float" || b < 0){
      return __flt_pow(_core_pow_float_arg(a), _core_pow_float_arg(b))
   }
   __bigint_pow(a, b)
}

@jit
fn eq(any a, any b) bool {
   "Checks structural equality between any two values."
   core_ref.eq(a, b)
}

@jit
fn ne(any a, any b) bool {
   "Checks structural inequality."
   !core_ref.eq(a, b)
}

fn contains(any container, any item) bool {
   "Checks if a collection(list, dict, set, string) contains an item."
   core_ref.contains(container, item)
}

fn startswith(any s, any prefix) bool {
   "Checks whether string `s` starts with string `prefix`."
   core_str.startswith(s, prefix)
}

fn endswith(any s, any suffix) bool {
   "Checks whether string `s` ends with string `suffix`."
   core_str.endswith(s, suffix)
}

fn type(any x) str {
   "Returns a string describing the runtime value type."
   if(__is_int(x)){ return "int" }
   if(x == true || x == false){ return "bool" }
   if(is_float(x)){ return "float" }
   if(is_str(x)){ return "str" }
   if(is_list(x)){ return "list" }
   if(is_dict(x)){ return "dict" }
   if(is_set(x)){ return "set" }
   if(is_tuple(x)){ return "tuple" }
   if(is_range(x)){ return "range" }
   if(is_bytes(x)){ return "bytes" }
   if(_core_has_tag(x, runtime_tag_raw("bigint"))){ return "bigint" }
   if(_core_has_tag(x, runtime_tag_raw("complex"))){ return "complex" }
   if(__tagof(x) == runtime_tag_raw("ffi_ptr")){ return "ffi_ptr" }
   if(is_ptr(x)){ return "ptr" }
   if(!x){ return "none" }
   "unknown"
}

fn type_shape(any x, int max_depth=6) str {
   "Returns a recursive runtime shape for containers, such as list<list<int>>."
   core_ref.type_shape(x, max_depth)
}

fn is_shape(any x, any spec, int max_depth=6) bool {
   "Returns true when `x` has the recursive runtime shape `spec`."
   core_ref.is_shape(x, spec, max_depth)
}

fn require_shape(any x, any spec, str msg="shape check failed", int max_depth=6) any {
   "Returns `x` when it has shape `spec`; otherwise panics with expected and actual shapes."
   core_ref.require_shape(x, spec, msg, max_depth)
}

fn assert_shape(any x, any spec, str msg="shape check failed", int max_depth=6) any {
   "Alias for require_shape, useful for tests and boundary checks."
   core_ref.assert_shape(x, spec, msg, max_depth)
}

fn items(any x) list {
   "Returns key-value pairs for dicts, [index, value] pairs for sequences."
   core_ref.items(x)
}

fn keys(any x) list {
   "Returns keys for dicts and index lists for sequences."
   core_ref.keys(x)
}

fn values(any x) list {
   "Returns values for dicts and elements for sequences."
   core_ref.values(x)
}

fn set(any obj=8, any key=nil, any val=nil) any {
   "Creates a set with set()/set(cap), or stores val with obj.set(key, val)."
   if(key == nil && val == nil && is_int(obj)){ return core_set.set(obj) }
   core_ref.set(obj, key, val)
}

fn put(any obj, any key, any val) any {
   "Stores `val` at `key` in a dict or sequence-like container."
   core_ref.set(obj, key, val)
}

fn set_idx(any obj, any key, any val) any {
   "Stores `val` at `key` in a dict or sequence-like container."
   core_ref.set_idx(obj, key, val)
}

fn delete(any obj, any key) any {
   "Removes `key` from a dict or set."
   if(is_dict(obj)){ return dict_remove(obj, key) }
   if(is_set(obj)){ return _set_remove(obj, key) }
   obj
}

fn replace(any x, any old, any new) any {
   "Returns a copy of `x` with matching items replaced."
   if(is_str(x)){
      return core_str.str_replace(x, old, new)
   }
   if(is_list(x) || is_tuple(x)){
      mut out = clone(x)
      mut i = 0
      def n = out.len
      while(i < n){
         if(eq(out.get(i, 0), old)){ out[i] = new }
         i += 1
      }
      return out
   }
   x
}

fn join(any items, any sep="") str {
   "Joins a list of values using separator `sep`."
   core_str.join(items, sep)
}

@returns_owned
fn Counter(any xs=[]) dict {
   "Builds a frequency counter dictionary from `xs`."
   collections.Counter(xs)
}

@returns_owned
fn counter(any xs=[]) dict {
   "Builds a frequency counter dictionary from `xs`."
   collections.counter(xs)
}

@returns_owned
@consumes(d)
fn counter_add(dict d, any key, int n=1) dict {
   "Adds `n` to `d[key]` and returns `d`."
   collections.counter_add(d, key, n)
}

@returns_owned
@consumes(d)
fn counter_inc(dict d, any key) dict {
   "Increments `d[key]` and returns `d`."
   collections.counter_inc(d, key)
}

@returns_owned
@consumes(d)
fn counter_update(dict d, any xs) dict {
   "Adds all values from `xs` into counter `d`."
   collections.counter_update(d, xs)
}

@returns_owned
fn count_by(any xs, fnptr key_fn) dict {
   "Counts values from `xs` by `key_fn(value)`."
   collections.count_by(xs, key_fn)
}

@returns_owned
fn most_common(dict d, int n=0) list {
   "Returns the most common counter entries."
   collections.most_common(d, n)
}

@returns_owned
fn group_by(any xs, fnptr key_fn) dict {
   "Groups values from `xs` by `key_fn(value)`."
   collections.group_by(xs, key_fn)
}

fn default_get(dict d, any key, any default) any {
   "Returns `d[key]`, inserting `default` into `d` when absent."
   collections.default_get(d, key, default)
}

@returns_owned
fn Queue(any xs=[]) dict {
   "Creates a FIFO queue."
   collections.Queue(xs)
}

@returns_owned
fn queue(any xs=[]) dict {
   "Creates a FIFO queue."
   collections.queue(xs)
}

@returns_owned
@consumes(q)
fn queue_push(dict q, any value) dict {
   "Pushes a value onto a queue."
   collections.queue_push(q, value)
}

fn queue_pop(dict q, any default=0) any {
   "Pops the next queue value."
   collections.queue_pop(q, default)
}

@returns_owned
fn queue_try_pop(dict q) dict {
   "Nonblocking queue pop returning `{ok, value, queue}`."
   collections.queue_try_pop(q)
}

fn queue_peek(dict q, any default=0) any {
   "Peeks at the next queue value."
   collections.queue_peek(q, default)
}

fn queue_len(dict q) int {
   "Returns queue length."
   collections.queue_len(q)
}

fn queue_empty(dict q) bool {
   "Returns true when a queue is empty."
   collections.queue_empty(q)
}

@returns_owned
@consumes(q)
fn queue_clear(dict q) dict {
   "Clears a queue."
   collections.queue_clear(q)
}

@returns_owned
fn Channel(int capacity=0) dict {
   "Creates a cooperative channel."
   collections.Channel(capacity)
}

@returns_owned
fn channel(int capacity=0) dict {
   "Creates a cooperative channel."
   collections.channel(capacity)
}

@returns_owned
fn chan(int capacity=0) dict {
   "Creates a cooperative channel."
   collections.chan(capacity)
}

fn chan_send(dict ch, any value) bool {
   "Sends a value to a cooperative channel."
   collections.chan_send(ch, value)
}

fn chan_try_send(dict ch, any value) bool {
   "Nonblocking channel send returning false when closed or full."
   collections.chan_try_send(ch, value)
}

fn chan_recv(dict ch, any default=0) any {
   "Receives a value from a cooperative channel."
   collections.chan_recv(ch, default)
}

@returns_owned
fn chan_try_recv(dict ch) dict {
   "Nonblocking channel receive returning `{ok, value, closed}`."
   collections.chan_try_recv(ch)
}

@returns_owned
@consumes(ch)
fn chan_close(dict ch) dict {
   "Closes a cooperative channel."
   collections.chan_close(ch)
}

fn chan_closed(dict ch) bool {
   "Returns true when a channel is closed."
   collections.chan_closed(ch)
}

fn chan_len(dict ch) int {
   "Returns queued channel messages."
   collections.chan_len(ch)
}

@jit
fn hash(any x) int {
   "Computes structural hash of a value."
   core_ref.hash(x)
}

@inline
fn repr(any x) str {
   "Returns the structural string representation of a value(quotes for strings, etc)."
   core_ref.repr(x)
}

fn is_truthy(any x) bool {
   "Returns **true** if value `x` is considered 'truthy'.
   - `none`: false
   - `int`: true if not 0
   - `str/list/dict/set/tuple`: true if length > 0
   - `other`: true"
   if(!x){ return false }
   if(__is_int(x)){ return x != 0 }
   if(is_str(x) || is_list(x) || is_dict(x) || is_set(x) || is_tuple(x)){ return core_ref.len(x) > 0 }
   true
}

@inline
fn is_falsy(any x) bool {
   "Returns **true** if value `x` is 'falsy' (not truthy)."
   !is_truthy(x)
}

@inline
fn not_none(any x, any fallback) any {
   "Returns `x` if it is not **none**; otherwise returns `fallback`."
   if(x != 0 || __is_int(x)){ return x }
   fallback
}

@inline
fn min(any a, any b) any {
   "Returns the smaller of `a` and `b`."
   a < b ? a : b
}

@inline
fn max(any a, any b) any {
   "Returns the larger of `a` and `b`."
   a > b ? a : b
}

@inline
fn sqrt(any x) f64 {
   "Returns the square root of `x`."
   __flt_sqrt(float(x))
}

@inline
fn panic(any msg) any {
   "Raises a panic with `msg`."
   __panic(msg)
}

@inline
fn ok(any v) any {
   "Creates an Ok result."
   __result_ok(v)
}

@inline
fn err(any e) any {
   "Creates an Err result."
   __result_err(e)
}

@inline
fn is_ok(any v) bool {
   "Returns true when v is an Ok result."
   __is_ok(v)
}

@inline
fn is_err(any v) bool {
   "Returns true when v is an Err result."
   __is_err(v)
}

@inline
fn unwrap(any v) any {
   "Returns the Ok payload, or panics on Err."
   if(__is_err(v)){ panic("unwrapped an Err: " + __to_str(__unwrap(v))) }
   __unwrap(v)
}

@inline
fn unwrap_or(any v, any default) any {
   "Returns the Ok payload, or default for Err/non-Ok."
   __is_ok(v) ? __unwrap(v) : default
}

@inline
fn panic_if(any cond, str msg) any {
   "Panics with `msg` if `cond` is truthy."
   if(is_truthy(cond)){ panic(msg) }
}

fn print(...args) int {
   "Prints one or more values to stdout, separated by spaces and ending with a newline(buffered)."
   mut i = 0
   def n = args.len
   mut line = ""
   mut wrote = false
   while(i < n){
      def v = args.get(i)
      if(!is_none(v)){
         def s = to_str(v)
         if(print_to_stdout){
            if(wrote){ __print_str_raw(" ") }
            if(__is_int(v)){ __print_int(v) }
            else { __print_str_raw(s) }
         }
         line = wrote ? (line + " " + s) : s
         wrote = true
      }
      i += 1
   }
   if(print_to_stdout){ __print_newline() }
   _append_print_history(line)
   0
}

fn _eprint_raw(any s) int {
   if(!is_str(s)){ return 0 }
   def n = load64(s, -16)
   if(n > 0){ __write_off(2, s, n, 0) }
   0
}

fn eprint(...args) int {
   "Prints one or more values to stderr, separated by spaces and ending with a newline."
   mut i = 0
   def n = args.len
   mut line = ""
   while(i < n){
      def s = to_str(args.get(i))
      _eprint_raw(s)
      line = (i > 0) ? (line + " " + s) : s
      if(i < n - 1){ _eprint_raw(" ") }
      i += 1
   }
   _eprint_raw("\n")
   _append_print_history(line)
   0
}

@jit
@inline
fn bool(any x) bool {
   "Converts `x` to a primitive boolean value."
   !!x
}

@inline
fn init_str(any p, int n) str {
   "Initializes a raw memory block `p` of size `n` as a Nytrix string object."
   init_str_raw(p, n)
}

@jit
fn load8(any p, int i=0) int {
   "Loads a single byte from address `p + i`."
   __load8_idx(p, i)
}

@jit
fn load64(any p, int i=0) any {
   "Loads a 64-bit integer from address `p + i`."
   __load64_idx(p, i)
}

@jit
fn load32_h(any p, int i=0) int {
   "Loads a 32-bit handle/scalar from address `p + i` and tags it as a Nytrix integer."
   def any: addr = p
   __load32_h(addr, i)
}

@jit
fn load64_h(any p, int i=0) int {
   "Loads a 64-bit handle and tags it as a Nytrix integer."
   def any: addr = p
   __load64_h(addr, i)
}

@jit
fn load64_i(any p, int i=0) int {
   "Loads a raw signed 64-bit scalar and returns it as an ordinary integer."
   def any: addr = p
   __load64_h(addr, i)
}

@jit
fn store8(any p, int v, int i=0) any {
   "Stores byte `v` at address `p + i`."
   __store8_idx(p, i, v)
}

@jit
fn store64(any p, any v, int i=0) any {
   "Stores 64-bit integer `v` at address `p + i`."
   __store64_idx(p, i, v)
}

@jit
fn store32_h(any p, any v, int i=0) any {
   "Stores a 32-bit handle/scalar `v` at address `p + i`, untagging integers."
   def any: addr = p
   __store32_idx(addr, i, v)
}

@jit
fn store64_h(any p, any v, int i=0) any {
   "Stores a 64-bit handle or value `v` at address `p + i`, untagging integers."
   def any: addr = p
   __store64_h(addr, i, v)
}

@jit
fn store64_i(any p, any v, int i=0) any {
   "Stores an ordinary integer as a raw signed 64-bit scalar."
   def any: addr = p
   __store64_h(addr, i, v)
}

@jit
fn load16(any p, int i=0) int {
   "Loads a 16-bit integer from address `p + i`."
   __load16_idx(p, i)
}

@jit
fn load32(any p, int i=0) int {
   "Loads a 32-bit integer from address `p + i`."
   __load32_idx(p, i)
}

@jit
fn store16(any p, int v, int i=0) any {
   "Stores a 16-bit integer `v` at address `p + i`."
   __store16_idx(p, i, v)
}

@jit
fn store32(any p, int v, int i=0) any {
   "Stores a 32-bit integer `v` at address `p + i`."
   __store32_idx(p, i, v)
}

@jit
fn load32_f32(any p, int i=0) any {
   "Loads a 32-bit float from address `p + i` and boxes it."
   __flt_box_val32(__load32_idx(p, i))
}

@jit
fn store32_f32(any p, any v, int i=0) any {
   "Unboxes float `v` and stores it as a 32-bit float at address `p + i`."
   if(!p){ return 0 }
   if(__is_int(v)){
      __store32_idx(p, i, __flt_unbox_val32(__flt_box_val(__flt_from_int(v))))
      return 0
   }
   if(__is_float_obj(v)){
      __store32_idx(p, i, __flt_unbox_val32(v))
      return 0
   }
   __store32_idx(p, i, 0)
}

@inline
fn load64_f64(any p, int i=0) any {
   "Loads a 64-bit float(double) from address `p + i` and boxes it."
   __flt_box_val(__load64_idx(p, i))
}

@inline
fn store64_f64(any p, any v, int i=0) any {
   "Unboxes float `v` and stores it as a 64-bit float(double) at address `p + i`."
   __store64_idx(p, i, __flt_unbox_val(v))
}

@jit
fn memcpy(any dst, any src, int n) any {
   "Copies `n` bytes from `src` to `dst`."
   __memcpy(dst, src, n)
}

@jit
fn memset(any dst, int v, int n) any {
   "Sets `n` bytes at `dst` to value `v`."
   __memset(dst, v, n)
}

@jit
fn memcmp(any a, any b, int n) int {
   "Compares `n` bytes of memory blocks `a` and `b`."
   __memcmp(a, b, n)
}

fn memchr(any p, int c, int n) any {
   "Searches for byte `c` in the first `n` bytes of `p`."
   core_mem.memchr(p, c, n)
}

@jit
fn ptr_add(any p, int n) any {
   "Returns address `p + n`."
   __add(p, n)
}

@jit
fn ptr_sub(any p, int n) any {
   "Returns address `p - n`."
   __sub(p, n)
}

@returns_owned
fn malloc(int n) ptr {
   "Allocates `n` bytes of heap memory."
   __malloc(n)
}

fn free(...ptrs) any {
   "Frees one or more heap memory blocks. Nil/zero pointers are ignored."
   mut i = 0
   while(i < ptrs.len){
      def p = ptrs.get(i)
      if(p){ __free(p) }
      i += 1
   }
   nil
}

@returns_owned
fn malloc_raw(int n) ptr {
   "Allocates unmanaged raw bytes for performance/FFI buffers. Free with free_raw."
   __malloc_raw(n)
}

fn free_raw(ptr p) any {
   "Frees memory allocated by malloc_raw."
   __free_raw(p)
}

fn retain(any x) any {
   "RC heap helper: retain `x` when running with --heap=rc."
   __retain_owned(x)
}

fn rc_count(any x) int {
   "RC heap debug helper: returns the current retain count for `x` under --heap=rc."
   __rc_count(x)
}

@borrows(x)
@returns_borrow(x)
fn borrow(any x) any {
   "Ownership mode helper: pass `x` without moving ownership."
   x
}

@returns_owned
@consumes(x)
fn own(any x) any {
   "Ownership mode helper: adopt/assert ownership of `x`."
   x
}

@consumes(x)
@releases(x)
fn release(any x) int {
   "Ownership mode helper: drop `x` now and disable later automatic cleanup."
   __drop_owned(x)
   0
}

@consumes(x)
@forgets(x)
fn forget(any x) int {
   "Ownership mode helper: intentionally leak or externally transfer `x`."
   0
}

fn realloc(any p, int n) ptr {
   "Resizes heap memory block `p` to `n` bytes."
   __realloc(p, n)
}

@returns_owned
fn zalloc(int n) ptr {
   "Allocates `n` bytes of heap memory and zeroes it out."
   def p = malloc(n)
   if(!p){ return 0 }
   memset(p, 0, n)
   p
}

@returns_owned
fn bytes(int n) bytes {
   "Allocates a native bytes buffer of length `n`."
   if(n < 0){ n = 0 }
   def p = bytes_new_raw(n)
   if(!p){ panic("bytes allocation failed") }
   p
}

fn bytes_get(bytes b, int i) int {
   "Returns byte at index `i`, or 0 for invalid access."
   if(!is_bytes(b)){ return 0 }
   if(i < 0 || i >= b.len){ return 0 }
   load8(b, i)
}

fn bytes_set(bytes b, int i, int v) bytes {
   "Stores byte `v` at index `i` and returns the same buffer."
   if(!is_bytes(b)){ return b }
   if(i < 0 || i >= b.len){ return b }
   store8(b, v, i)
   b
}

@returns_owned
fn list(int cap=8) list {
   "Creates a new empty list with initial capacity `cap`."
   def p = __list_new(cap)
   if(!p){ panic("list allocation failed") }
   p
}

comptime template _core_ref_ctor2(name){ fn ${name}(x=0, y=nil) { core_ref.${name}(x, y) } }

comptime template _core_ref_ctor3(name){ fn ${name}(x=0, y=nil, z=nil) { core_ref.${name}(x, y, z) } }

comptime template _core_ref_ctor4(name){ fn ${name}(x=0, y=nil, z=nil, w=nil) { core_ref.${name}(x, y, z, w) } }

comptime emit _core_ref_ctor2(Vector2)
comptime emit _core_ref_ctor3(Vector3)
comptime emit _core_ref_ctor4(Vector4)
comptime emit _core_ref_ctor2(vec2)
comptime emit _core_ref_ctor3(vec3)
comptime emit _core_ref_ctor4(vec4)

fn is_nytrix_obj(any x) bool {
   "Returns **true** if `x` is a Nytrix-managed heap object."
   __is_ny_obj(x)
}

comptime template _core_tag_pred(name, tag){
   @jit
   fn ${name}(any x) bool { _core_has_tag(x, ${tag}) }
}

@inline
fn _core_has_tag(any x, any tag) bool {
   def got = __tagof(x)
   got == tag || got == __tag(tag)
}

def _CORE_TAG_LIST = runtime_tag_raw("list")
def _CORE_TAG_DICT = runtime_tag_raw("dict")
def _CORE_TAG_SET = runtime_tag_raw("set")
def _CORE_TAG_TUPLE = runtime_tag_raw("tuple")
def _CORE_TAG_RANGE = runtime_tag_raw("range")
def _CORE_TAG_BYTES = runtime_tag_raw("bytes")
comptime emit _core_tag_pred(is_list, _CORE_TAG_LIST)
comptime emit _core_tag_pred(is_dict, _CORE_TAG_DICT)
comptime emit _core_tag_pred(is_set, _CORE_TAG_SET)
comptime emit _core_tag_pred(is_tuple, _CORE_TAG_TUPLE)
comptime emit _core_tag_pred(is_range, _CORE_TAG_RANGE)
comptime emit _core_tag_pred(is_bytes, _CORE_TAG_BYTES)

@jit
fn is_str(any x) bool {
   "Returns **true** if `x` is a string."
   __is_str_obj(x)
}

@jit
fn is_float(any x) bool {
   "Returns **true** if `x` is a float object."
   __is_float_obj(x)
}

@inline
fn to_int(any v) any {
   "Untags a Nytrix value into a raw integer."
   if(__is_int(v)){ return __untag(v) }
   v
}

@inline
fn from_int(int v) int {
   "Tags a raw integer `v` into a Nytrix value."
   __tag(v)
}

@inline
fn to_str(any v) str {
   "Converts `v` to a string."
   core_ref.to_str(v)
}

@inline
fn _core_bytes_like_len(any x) int { load64(x, -16) }

@returns_owned
fn _core_bytes_like_to_list(any x) list<int> {
   mut out = []
   mut i = 0
   def n = _core_bytes_like_len(x)
   while(i < n){
      out = out.append(load8(x, i) & 255)
      i += 1
   }
   out
}

fn _core_hex_nibble(int c) int {
   return case c {
      48..57 -> c - 48
      65..70 -> c - 55
      97..102 -> c - 87
      _ -> 0
   }
}

@returns_owned
fn _core_unhex(str hex) list<int> {
   def n = _core_bytes_like_len(hex)
   if(n <= 0){ return [] }
   mut out = []
   mut i = 0
   if((n % 2) == 1){
      out = out.append(_core_hex_nibble(load8(hex, 0)))
      i = 1
   }
   while(i < n){
      def hi = _core_hex_nibble(load8(hex, i))
      def lo = _core_hex_nibble(load8(hex, i + 1))
      out = out.append((hi << 4) | lo)
      i += 2
   }
   out
}

@returns_owned
fn _core_bigint_to_bytes(bigint x) list<int> {
   __bigint_to_bytes(x)
}

fn atoi(any s) int {
   "Parses a base-10 integer from string `s`."
   if(!is_str(s)){ return 0 }
   def n = _core_bytes_like_len(s)
   if(n == 0){ return 0 }
   mut sign = 1
   mut i = 0
   if(load8(s, 0) == 45){
      sign = -1
      i = 1
   }
   mut out = 0
   while(i < n){
      def c = load8(s, i)
      if(c < 48 || c > 57){ break }
      out = out * 10 + (c - 48)
      i += 1
   }
   out * sign
}

@inline
fn str(any v) str {
   "Converts `v` to a string. Canonical cast spelling for string values."
   to_str(v)
}

@returns_owned
fn chr(int code) str {
   "Returns a single-character string from a Unicode code point."
   use std.core.str
   if(code < 0 || code > 1114111){ return "" }
   def char_buf = malloc(5)
   if(!char_buf){ return "" }
   mut n = 0
   if(code <= 127){
      store8(char_buf, code, 0)
      n = 1
   } elif(code <= 2047){
      store8(char_buf, 192 | (code >> 6), 0)
      store8(char_buf, 128 | (code & 63), 1)
      n = 2
   } elif(code <= 65535){
      store8(char_buf, 224 | (code >> 12), 0)
      store8(char_buf, 128 | ((code >> 6) & 63), 1)
      store8(char_buf, 128 | (code & 63), 2)
      n = 3
   } else {
      store8(char_buf, 240 | (code >> 18), 0)
      store8(char_buf, 128 | ((code >> 12) & 63), 1)
      store8(char_buf, 128 | ((code >> 6) & 63), 2)
      store8(char_buf, 128 | (code & 63), 3)
      n = 4
   }
   store8(char_buf, 0, n)
   def out = core_str.cstr_to_str(char_buf)
   free(char_buf)
   out
}

@returns_owned
fn __kwarg(any k, any v) any {
   def p = kwarg_new_raw(k, v)
   if(!p){ panic("kwarg allocation failed") }
   p
}

fn is_kwargs(any x) bool {
   "Returns **true** if `x` is a keyword argument wrap object."
   if(!x || __is_int(x)){ return false }
   _core_has_tag(x, runtime_tag_raw("kwarg"))
}

@returns_owned
fn kwarg(any k, any v) any {
   "Creates a keyword-argument pair."
   __kwarg(k, v)
}

fn get_kwarg_key(any x) any {
   "Returns keyword-argument key."
   load64(x, 0)
}

fn get_kwarg_val(any x) any {
   "Returns keyword-argument value."
   load64(x, 8)
}

fn get(any obj, any key, any default=0) any {
   "Gets item/key with fallback."
   core_ref.get(obj, key, default)
}

fn index_read(any obj, any key) any {
   "Strict index read for `obj[key]`."
   core_ref.index_read(obj, key)
}

fn slice(any obj, int start, any stop, int step=1) any {
   "Returns a sliced copy."
   core_ref.slice(obj, start, stop, step)
}

fn append(any lst, any v) any {
   "Appends value to list-like container."
   return core_ref.append(lst, v)
}

fn pop(any lst) any {
   "Pops last value from list-like container."
   core_ref.pop(lst)
}

fn extend(any lst, any other) any {
   "Extends list-like container."
   core_ref.extend(lst, other)
}

@jit
fn len(any x) int {
   "Returns the number of elements in a collection or the length of a string."
   if(is_list(x) || is_dict(x) || is_set(x) || is_tuple(x)){ return __load64_idx(x, 0) }
   if(is_str(x) || is_bytes(x)){ return __load64_idx(x, -16) }
   core_ref.len(x)
}

impl any {
   @inline
   fn long(any self) bigint { __long(self) }
   @inline
   fn to_bytes(any self) list {
      if(is_list(self)){ return self }
      if(is_bytes(self) || is_str(self)){ return _core_bytes_like_to_list(self) }
      []
   }
   @inline
   fn as_bytes(any self) list {
      if(is_int(self)){ return _core_bigint_to_bytes(__bigint_from_int(self)) }
      self.to_bytes
   }
   @inline
   fn unhex(any self) list<int> {
      if(is_str(self)){ return _core_unhex(self) }
      []
   }
   @inline
   fn type_shape(any self, int max_depth=6) str { core_ref.type_shape(self, max_depth) }
   @inline
   fn is_shape(any self, any spec, int max_depth=6) bool { core_ref.is_shape(self, spec, max_depth) }
   @inline
   fn require_shape(any self, any spec, str msg="shape check failed", int max_depth=6) any { core_ref.require_shape(self, spec, msg, max_depth) }
   @inline
   fn len(any self) int {
      if(is_list(self) || is_dict(self) || is_set(self) || is_tuple(self)){ return __load64_idx(self, 0) }
      if(is_str(self) || is_bytes(self)){ return __load64_idx(self, -16) }
      core_ref.len(self)
   }
   @inline
   fn get(any self, any key, any default=0) any { core_ref.get(self, key, default) }
   @inline
   fn set(any self, any key, any val) any { core_ref.set(self, key, val) }
   @inline
   fn put(any self, any key, any val) any { core_ref.set(self, key, val) }
   @inline
   fn add(any self, any val) any {
      if(is_set(self)){ return _set_add(self, val) }
      if(is_list(self)){ return core_ref.append(self, val) }
      core_ref.add(self, val)
   }
   @inline
   fn append(any self, any val) any { return core_ref.append(self, val) }
   @inline
   fn pop(any self) any { core_ref.pop(self) }
   @inline
   fn extend(any self, any other) any { return core_ref.extend(self, other) }
   @inline
   fn contains(any self, any item) bool { core_ref.contains(self, item) }
   @inline
   fn slice(any self, int start, any stop, int step=1) any { core_ref.slice(self, start, stop, step) }
   @inline
   fn keys(any self) list { core_ref.keys(self) }
   @inline
   fn values(any self) list { core_ref.values(self) }
   @inline
   fn items(any self) list { core_ref.items(self) }
   @inline
   fn merge(any self, any other) any { dict_merge(self, other) }
   @inline
   fn map(any self, fnptr f) any { core_iter.map(self, f) }
   @inline
   fn filter(any self, fnptr pred) any { core_iter.filter(self, pred) }
   @inline
   fn reduce(any self, any init, fnptr f) any { core_iter.reduce(self, init, f) }
   @inline
   fn each(any self, fnptr f) any { core_iter.each(self, f) }
   @inline
   fn count(any self) int { core_iter.count(self) }
   @inline
   fn count_if(any self, fnptr pred) int { core_iter.count_if(self, pred) }
   @inline
   fn first(any self, any default=0) any { core_iter.first(self, default) }
   @inline
   fn last(any self, any default=0) any { core_iter.last(self, default) }
   @inline
   fn take(any self, int n) any { core_iter.take(self, n) }
   @inline
   fn drop(any self, int n) any { core_iter.drop(self, n) }
   @inline
   fn reverse(any self) any { core_iter.reverse(self) }
   @inline
   fn compact(any self) list { core_iter.compact(self) }
   @inline
   fn chunk(any self, int size) list { core_iter.chunk(self, size) }
   @inline
   fn windowed(any self, int size, int step=1) list { core_iter.windowed(self, size, step) }
   @inline
   fn delete(any self, any key) any {
      if(is_dict(self)){ return dict_remove(self, key) }
      if(is_set(self)){ return _set_remove(self, key) }
      self
   }
   @inline
   fn remove(any self, any key) any { self.delete(key) }
   @inline
   fn clear(any self) any {
      if(is_dict(self)){ return dict_clear(self) }
      if(is_set(self)){ return _set_clear(self) }
      if(is_list(self)){
         store64(self, 0, 0)
         return self
      }
      self
   }
}

impl bigint {
   @inline
   fn bytes(bigint self) list<int> { _core_bigint_to_bytes(self) }
   @inline
   fn as_bytes(bigint self) list<int> { _core_bigint_to_bytes(self) }
}

impl str {
   @inline
   fn long(str self) bigint { __long(self) }
   @inline
   fn to_bytes(str self) list<int> { _core_bytes_like_to_list(self) }
   @inline
   fn as_bytes(str self) list<int> { _core_bytes_like_to_list(self) }
   @inline
   fn unhex(str self) list<int> { _core_unhex(self) }
   @inline
   fn len(str self) int { core_ref.len(self) }
   @inline
   fn get(str self, any key, any default=0) any { core_ref.get(self, key, default) }
   @inline
   fn contains(str self, any item) bool { core_ref.contains(self, item) }
   @inline
   fn slice(str self, int start, any stop, int step=1) any { core_ref.slice(self, start, stop, step) }
   @inline
   fn map(str self, fnptr f) any { core_iter.map(self, f) }
   @inline
   fn filter(str self, fnptr pred) any { core_iter.filter(self, pred) }
   @inline
   fn reduce(str self, any init, fnptr f) any { core_iter.reduce(self, init, f) }
   @inline
   fn each(str self, fnptr f) any { core_iter.each(self, f) }
   @inline
   fn count(str self) int { core_iter.count(self) }
   @inline
   fn count_if(str self, fnptr pred) int { core_iter.count_if(self, pred) }
   @inline
   fn first(str self, any default=0) any { core_iter.first(self, default) }
   @inline
   fn last(str self, any default=0) any { core_iter.last(self, default) }
   @inline
   fn take(str self, int n) any { core_iter.take(self, n) }
   @inline
   fn drop(str self, int n) any { core_iter.drop(self, n) }
   @inline
   fn reverse(str self) any { core_iter.reverse(self) }
   @inline
   fn chunk(str self, int size) list { core_iter.chunk(self, size) }
   @inline
   fn windowed(str self, int size, int step=1) list { core_iter.windowed(self, size, step) }
}

impl dict {
   @inline
   fn len(dict self) int { core_ref.len(self) }
   @inline
   fn clone(dict self) dict { dict_clone(self) }
   @inline
   fn get(dict self, any key, any default=0) any { core_ref.get(self, key, default) }
   @inline
   fn set(dict self, any key, any val) dict { core_ref.set(self, key, val) }
   @inline
   fn put(dict self, any key, any val) dict { core_ref.set(self, key, val) }
   @inline
   fn contains(dict self, any key) bool { dict_exists(self, key) }
   @inline
   fn delete(dict self, any key) dict { dict_remove(self, key) }
   @inline
   fn clear(dict self) dict { dict_clear(self) }
   @inline
   fn keys(dict self) list { dict_keys(self) }
   @inline
   fn values(dict self) list { dict_values(self) }
   @inline
   fn items(dict self) list { dict_items(self) }
   @inline
   fn merge(dict self, dict other) dict { dict_merge(self, other) }
}

impl list {
   @inline
   fn long(list self) bigint { __long(self) }
   @inline
   fn to_bytes(list self) list { self }
   @inline
   fn as_bytes(list self) list { self }
   @inline
   fn bytes(list self) list { self }
   @inline
   fn len(list self) int { core_ref.len(self) }
   @inline
   fn get(list self, any key, any default=0) any {
      if(__is_int(key)){
         mut k = key
         def n = __load64_idx(self, 0)
         if(__lt(k, 0)){ k = __add(k, n) }
         if(__lt(k, 0) || __ge(k, n)){ return default }
         return __load_item(self, k)
      }
      core_ref.get(self, key, default)
   }
   @inline
   fn set(list self, any key, any val) list { core_ref.set(self, key, val) }
   @inline
   fn put(list self, any key, any val) list { core_ref.set(self, key, val) }
   @inline
   fn add(list self, any val) list { return core_ref.append(self, val) }
   @inline
   fn append(list self, any val) list { return core_ref.append(self, val) }
   @inline
   fn pop(list self) any { core_ref.pop(self) }
   @inline
   fn extend(list self, any other) list { return core_ref.extend(self, other) }
   @inline
   fn clear(list self) list {
      store64(self, 0, 0)
      self
   }
   @inline
   fn contains(list self, any item) bool { core_ref.contains(self, item) }
   @inline
   fn join(list self, str sep="") str { core_str.join(self, sep) }
   @inline
   fn slice(list self, int start, any stop, int step=1) any { core_ref.slice(self, start, stop, step) }
   @inline
   fn map(list self, fnptr f) any { core_iter.map(self, f) }
   @inline
   fn filter(list self, fnptr pred) any { core_iter.filter(self, pred) }
   @inline
   fn reduce(list self, any init, fnptr f) any { core_iter.reduce(self, init, f) }
   @inline
   fn each(list self, fnptr f) any { core_iter.each(self, f) }
   @inline
   fn count(list self) int { core_iter.count(self) }
   @inline
   fn count_if(list self, fnptr pred) int { core_iter.count_if(self, pred) }
   @inline
   fn first(list self, any default=0) any { core_iter.first(self, default) }
   @inline
   fn last(list self, any default=0) any { core_iter.last(self, default) }
   @inline
   fn take(list self, int n) any { core_iter.take(self, n) }
   @inline
   fn drop(list self, int n) any { core_iter.drop(self, n) }
   @inline
   fn reverse(list self) any { core_iter.reverse(self) }
   @inline
   fn compact(list self) list { core_iter.compact(self) }
   @inline
   fn chunk(list self, int size) list { core_iter.chunk(self, size) }
   @inline
   fn windowed(list self, int size, int step=1) list { core_iter.windowed(self, size, step) }
}

impl tuple {
   @inline
   fn len(tuple self) int { core_ref.len(self) }
   @inline
   fn get(tuple self, any key, any default=0) any { core_ref.get(self, key, default) }
   @inline
   fn contains(tuple self, any item) bool { core_ref.contains(self, item) }
   @inline
   fn map(tuple self, fnptr f) any { core_iter.map(self, f) }
   @inline
   fn filter(tuple self, fnptr pred) any { core_iter.filter(self, pred) }
   @inline
   fn reduce(tuple self, any init, fnptr f) any { core_iter.reduce(self, init, f) }
   @inline
   fn count(tuple self) int { core_iter.count(self) }
   @inline
   fn count_if(tuple self, fnptr pred) int { core_iter.count_if(self, pred) }
   @inline
   fn first(tuple self, any default=0) any { core_iter.first(self, default) }
   @inline
   fn last(tuple self, any default=0) any { core_iter.last(self, default) }
   @inline
   fn take(tuple self, int n) any { core_iter.take(self, n) }
   @inline
   fn drop(tuple self, int n) any { core_iter.drop(self, n) }
   @inline
   fn reverse(tuple self) any { core_iter.reverse(self) }
   @inline
   fn compact(tuple self) list { core_iter.compact(self) }
   @inline
   fn chunk(tuple self, int size) list { core_iter.chunk(self, size) }
   @inline
   fn windowed(tuple self, int size, int step=1) list { core_iter.windowed(self, size, step) }
}

impl set {
   @inline
   fn len(set self) int { core_ref.len(self) }
   @inline
   fn add(set self, any key) set { _set_add(self, key) }
   @inline
   fn sub(set self, any key) set { _set_remove(self, key) }
   @inline
   fn remove(set self, any key) set { _set_remove(self, key) }
   @inline
   fn delete(set self, any key) set { _set_remove(self, key) }
   @inline
   fn contains(set self, any key) bool { _set_contains(self, key) }
   @inline
   fn clear(set self) set { _set_clear(self) }
   @inline
   fn values(set self) list { _set_values(self) }
}

impl bytes {
   @inline
   fn long(bytes self) bigint { __long(self) }
   @inline
   fn to_list(bytes self) list<int> { _core_bytes_like_to_list(self) }
   @inline
   fn to_bytes(bytes self) list<int> { _core_bytes_like_to_list(self) }
   @inline
   fn as_bytes(bytes self) list<int> { _core_bytes_like_to_list(self) }
   @inline
   fn bytes(bytes self) list<int> { _core_bytes_like_to_list(self) }
   @inline
   fn len(bytes self) int { core_ref.len(self) }
   @inline
   fn get(bytes self, int key, any default=0) any { core_ref.get(self, key, default) }
   @inline
   fn set(bytes self, int key, int val) bytes { core_ref.set(self, key, val) }
   @inline
   fn put(bytes self, int key, int val) bytes { self.set(key, val) }
}

impl range {
   @inline
   fn len(range self) int { core_ref.len(self) }
   @inline
   fn get(range self, any key, any default=0) any { core_ref.get(self, key, default) }
   @inline
   fn keys(range self) list { core_ref.keys(self) }
   @inline
   fn values(range self) list { core_ref.values(self) }
   @inline
   fn items(range self) list { core_ref.items(self) }
   @inline
   fn map(range self, fnptr f) any { core_iter.map(self, f) }
   @inline
   fn filter(range self, fnptr pred) any { core_iter.filter(self, pred) }
   @inline
   fn reduce(range self, any init, fnptr f) any { core_iter.reduce(self, init, f) }
   @inline
   fn count(range self) int { core_iter.count(self) }
   @inline
   fn count_if(range self, fnptr pred) int { core_iter.count_if(self, pred) }
   @inline
   fn first(range self, any default=0) any { core_iter.first(self, default) }
   @inline
   fn last(range self, any default=0) any { core_iter.last(self, default) }
   @inline
   fn take(range self, int n) any { core_iter.take(self, n) }
   @inline
   fn drop(range self, int n) any { core_iter.drop(self, n) }
   @inline
   fn reverse(range self) any { core_iter.reverse(self) }
   @inline
   fn chunk(range self, int size) list { core_iter.chunk(self, size) }
   @inline
   fn windowed(range self, int size, int step=1) list { core_iter.windowed(self, size, step) }
   @inline
   fn contains(range self, any item) bool {
      core_ref.contains(self, item)
   }
}

@returns_owned
fn _clone_list(any lst) any {
   if(!is_list(lst)){ return 0 }
   def n = __load64_idx(lst, 0)
   mut list: out = list(n)
   mut i = 0
   while(i < n){
      out[i] = __load_item(lst, i)
      i += 1
   }
   __store64_idx(out, 0, n)
   out
}

@inline
fn clone(any x) any {
   "Returns a shallow copy of lists, tuples, and dicts.
   Strings are returned unchanged."
   if(is_list(x)){ return _clone_list(x) }
   if(is_tuple(x)){ return _sorted_list_copy(x) }
   if(is_str(x)){ return x }
   if(is_dict(x)){ return dict_clone(x) }
   x
}

@jit
fn load_item(any lst, int i) any {
   "Loads the item at index `i` from list `lst`."
   load64(lst, 16 + i * 8)
}

@jit
fn store_item(any lst, int i, any v) any {
   "Stores value `v` at index `i` in list `lst`."
   store64(lst, v, 16 + i * 8) v
}

fn swap_items(any lst, int i, int j) any {
   "Swaps elements at indices `i` and `j` in list `lst`.
   Supports negative indices and returns the list."
   if(!is_list(lst)){ return lst }
   def n = __load64_idx(lst, 0)
   if(i < 0){ i += n }
   if(j < 0){ j += n }
   if(i < 0 || i >= n || j < 0 || j >= n){ panic("swap index out of range") }
   if(i == j){ return lst }
   def tmp = __load_item(lst, i)
   lst[i] = __load_item(lst, j)
   lst[j] = tmp
   lst
}

@inline
fn swap(list lst, int i, int j) list {
   "Compatibility wrapper for list index swap."
   if(!is_list(lst)){ return lst }
   def n = __load64_idx(lst, 0)
   if(i < 0){ i += n }
   if(j < 0){ j += n }
   if(i < 0 || i >= n || j < 0 || j >= n){ panic("swap index out of range") }
   if(i == j){ return lst }
   def tmp = __load_item(lst, i)
   lst[i] = __load_item(lst, j)
   lst[j] = tmp
   lst
}

@returns_owned
fn swapped(any xs, int i, int j) any {
   "Returns a copy of `xs` with indices `i` and `j` exchanged.
   Lists return a new list ; tuples and strings preserve their type."
   if(is_list(xs)){
      if(xs.len <= 1){ return _clone_list(xs) }
      def out = _clone_list(xs)
      return swap_items(out, i, j)
   }
   if(is_tuple(xs)){
      if(xs.len <= 1){ return clone(xs) }
      def out = _sorted_list_copy(xs)
      swap_items(out, i, j)
      list_as_tuple_raw(out)
      return out
   }
   if(is_str(xs)){
      if(xs.len <= 1){ return xs }
      mut chars = _sorted_list_copy(xs)
      swap_items(chars, i, j)
      return _char_list_to_str(chars)
   }
   xs
}

@returns_owned
fn _sorted_list_copy(any xs) list {
   def n = xs.len
   mut out = list(n)
   mut i = 0
   while(i < n){
      out[i] = xs.get(i)
      i += 1
   }
   __store64_idx(out, 0, n)
   out
}

@returns_owned
fn _char_list_to_str(list chars) str {
   use std.core.str
   def n = chars.len
   mut out = Builder(n + 8)
   mut i = 0
   while(i < n){
      out = builder_append(out, chars.get(i))
      i += 1
   }
   def s = builder_to_str(out)
   builder_free(out)
   s
}

@returns_owned
fn _empty_tuple() tuple {
   mut out = list(0)
   list_as_tuple_raw(out)
}

fn sort(any xs) any {
   "Sorts sequences.
   - lists: sorts in place and returns the same list
   - strings: returns a sorted string copy
   - tuples: returns a sorted tuple copy
   - ranges: returns a sorted list copy"
   __sort_any(xs)
}

fn sorted(any xs) any {
   "Returns a sorted copy of `xs`.
   Lists and tuples are copied to lists before sorting ; strings return a sorted string copy."
   __sorted_any(xs)
}

fn clear(any x) any {
   "Clears a container.
   Lists, dicts, and sets are cleared in place.
   Strings and tuples return an empty value of the same outer shape."
   if(is_list(x)){
      store64(x, 0, 0)
      return x
   }
   if(is_dict(x)){ return dict_clear(x) }
   if(is_set(x)){ return _set_clear(x) }
   if(is_str(x)){ return "" }
   if(is_tuple(x)){ return _empty_tuple() }
   x
}

fn add(any a, any b) any {
   "Generic addition for primitives and objects.
   Also appends to lists and inserts into sets."
   if(is_list(a) && !(is_list(b) || is_tuple(b))){ return a.append(b) }
   if(is_set(a)){ return _set_add(a, b) }
   if(__is_int(a) && __is_int(b)){ return __add(a, b) }
   core_ref.add(a, b)
}

fn sub(any a, any b) any {
   "Generic subtraction for primitives and objects.
   Also removes `b` from sets."
   if(is_set(a)){ return _set_remove(a, b) }
   if(__is_int(a) && __is_int(b)){ return __sub(a, b) }
   core_ref.sub(a, b)
}

fn mul(any a, any b) any {
   "Generic multiplication for primitives and objects(handles bigint)."
   if(__is_int(a) && __is_int(b)){ return __mul(a, b) }
   core_ref.mul(a, b)
}

fn div(any a, any b) any {
   "Generic division for primitives and objects(handles bigint)."
   if(__is_int(a) && __is_int(b)){
      if(b == 0){ panic("division by zero") }
      return __div(a, b)
   }
   core_ref.div(a, b)
}

fn mod(any a, any b) any {
   "Generic modulus for primitives and objects(handles bigint)."
   __mod(a, b)
}

@jit
fn lt(any a, any b) bool {
   "Generic less-than for primitives and objects(handles bigint)."
   __lt(a, b)
}

@jit
fn le(any a, any b) bool {
   "Generic less-than-or-equal for primitives and objects(handles bigint)."
   __le(a, b)
}

@jit
fn gt(any a, any b) bool {
   "Generic greater-than for primitives and objects(handles bigint)."
   __gt(a, b)
}

@jit
fn ge(any a, any b) bool {
   "Generic greater-than-or-equal for primitives and objects(handles bigint)."
   __ge(a, b)
}

def OS = __os_name()
def ARCH = __arch_name()
def IS_LINUX   = linux
def IS_MACOS   = macos
def IS_WINDOWS = windows
def IS_X86_64  = x86_64
def IS_AARCH64 = aarch64
def IS_ARM     = arm

fn _pow2(int n) int {
   mut v = 1
   while(v < n){ v = v << 1 }
   v
}

#main {
   def xs = [1, 2, 3]
   def cloned = clone(xs)
   cloned[0] = 9
   assert(xs[0] == 1 && cloned[0] == 9, "core clone detaches list")
   swap(xs, 0, 2)
   assert(xs[0] == 3 && xs[2] == 1, "core swap indexed assignment")
   def ys = swapped([4, 5, 6], 0, 2)
   assert(ys[0] == 6 && ys[2] == 4, "core swapped copy")
   def d = dict().set("a", 1).set("b", 2)
   assert(d.keys.len == 2 && d.values.len == 2 && d.items.len == 2, "core dict item builders")
   assert(repr([1, "x"]) == "[1, \"x\"]", "core repr raw store")
   assert(to_str({"a": 1}).contains("a: 1"), "core dict to_str")
   print("✓ std.core self-test passed")
}
