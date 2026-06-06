;; Keywords: reflect reflection introspection core
;; Runtime type, shape, equality, and representation inspection.
;; References:
;; - std.core
module std.core.reflect(len, contains, type, type_shape, is_shape, require_shape, assert_shape, add, sub, mul, div, list_eq, dict_eq, set_eq, eq, repr, hash, globals, items, keys, values, get, index_read, set, set_idx, slice, append, pop, extend, to_str, vec2, vec3, vec4, Vector2, Vector3, Vector4)
use std.core.error
use std.core.str
use std.core.primitives
use std.core.primitives as prim
use std.core.dict_mod

def _TAG_FFI_PTR = runtime_tag_raw("ffi_ptr")
def _TAG_LIST = runtime_tag_raw("list")
def _TAG_DICT = runtime_tag_raw("dict")
def _TAG_SET = runtime_tag_raw("set")
def _TAG_TUPLE = runtime_tag_raw("tuple")
def _TAG_RANGE = runtime_tag_raw("range")
def _TAG_PTR = runtime_tag_raw("ptr")
def _TAG_FLOAT = runtime_tag_raw("float")
def _TAG_COMPLEX = runtime_tag_raw("complex")
def _TAG_STR = runtime_tag_raw("str")
def _TAG_STR_CONST = runtime_tag_raw("str_const")
def _TAG_BYTES = runtime_tag_raw("bytes")
def _TAG_BIGINT = runtime_tag_raw("bigint")

@inline
fn _has_tag(any x, any tag) bool {
   def got = __tagof(x)
   got == tag || got == __tag(tag)
}

@inline
fn _is_dict(any x) bool { _has_tag(x, _TAG_DICT) }

@inline
fn _is_list(any x) bool { _has_tag(x, _TAG_LIST) }

@inline
fn _is_set(any x) bool { _has_tag(x, _TAG_SET) }

@inline
fn _is_tuple(any x) bool { _has_tag(x, _TAG_TUPLE) }

@inline
fn _is_range(any x) bool { _has_tag(x, _TAG_RANGE) }

@inline
fn _is_bytes(any x) bool { _has_tag(x, _TAG_BYTES) }

@inline
fn _is_raw_ptr_like(any x) bool {
   def tag = __tagof(x)
   __eq(tag, _TAG_PTR) || __eq(tag, _TAG_FFI_PTR) || (tag == 0 && prim.is_ptr(x))
}

@inline
fn _is_seq_tag(any tag) bool {
   if(!__is_int(tag)){ return false }
   __eq(tag, _TAG_LIST) || __eq(tag, _TAG_TUPLE) || __eq(tag, _TAG_RANGE)
}

@inline
fn _is_list_tuple_tag(any tag) bool {
   if(!__is_int(tag)){ return false }
   __eq(tag, _TAG_LIST) || __eq(tag, _TAG_TUPLE)
}

@inline
fn _is_str_tag(any tag) bool {
   if(!__is_int(tag)){ return false }
   __eq(tag, _TAG_STR) || __eq(tag, _TAG_STR_CONST)
}

@inline
fn _is_seq(any x) bool { _is_seq_tag(__tagof(x)) }

@inline
fn _is_bigint(any x) bool { _has_tag(x, _TAG_BIGINT) }

@inline
fn _is_str(any x) bool { _is_str_tag(__tagof(x)) }

@inline
fn _is_float(any x) bool { __is_float_obj(x) }

@inline
fn _dict_get_raw(any d, any key, any default=0) any { dict_read(d, key, default) }

@inline
fn _dict_put_raw(any d, any key, any value) any { dict_write(d, key, value) }

@inline
fn _dict_has_raw(any d, any key) bool { dict_exists(d, key) }

@inline
fn _dict_items_raw(any d) list { dict_items(d) }

@inline
fn _dict_keys_raw(any d) list { dict_keys(d) }

@inline
fn _dict_values_raw(any d) list { dict_values(d) }

@inline
fn _set_seq_count(any xs, int count) any {
   store64(xs, count, 0)
   xs
}

@inline
fn _raw_len(any obj) int { __load64_idx(prim.sub(obj, 16), 0) }

@inline
fn _float_one() any { __flt_box_val(__flt_from_int(1)) }

@inline
fn _as_float(any x) any { __mul(x, _float_one()) }

@inline
fn _store_item_raw(list xs, int index, any value) list {
   __store_item_fast(xs, index, value)
   xs
}

fn _type_error(str op, str want, any got) any {
   __panic(op + " expects " + want + ", got " + type(got) + " (" + repr(got) + ")")
   0
}

fn _vec_dim_type(any t) int {
   case t {
      "vec2", "Vector2" -> 2
      "vec3", "Vector3" -> 3
      "vec4", "Vector4" -> 4
      _ -> 0
   }
}

fn _vec_kind_name(int n) str {
   return case n {
      2 -> "vec2"
      4 -> "vec4"
      _ -> "vec3"
   }
}

fn _is_vecdict(any x) bool {
   if(!_is_dict(x)){ return false }
   _vec_dim_type(_dict_get_raw(x, "__type", "")) > 0
}

fn _vec_dim(any x) int { _vec_dim_type(_dict_get_raw(x, "__type", "")) }

fn _seq_at(any x, int i, any default=0) any {
   def n = __load64_idx(x, 0)
   if(i < 0 || i >= n){ return default }
   __load_item(x, i)
}

fn _vec_at(any x, int i, any default=0) any {
   return case i {
      0 -> _dict_get_raw(x, "x", default)
      1 -> _dict_get_raw(x, "y", default)
      2 -> _dict_get_raw(x, "z", default)
      3 -> _dict_get_raw(x, "w", default)
      _ -> default
   }
}

fn _vec_set(any x, int i, any val) any {
   return case i {
      0 -> _dict_put_raw(x, "x", val)
      1 -> _dict_put_raw(x, "y", val)
      2 -> _dict_put_raw(x, "z", val)
      3 -> _dict_put_raw(x, "w", val)
      _ -> x
   }
}

fn _vec_make(int n, any x, any y=0, any z=0, any w=0) any {
   mut out = _dict_put_raw(dict(8), "__type", _vec_kind_name(n))
   out = _dict_put_raw(out, "x", x)
   out = _dict_put_raw(out, "y", y)
   if(n >= 3){ out = _dict_put_raw(out, "z", z) }
   if(n >= 4){ out = _dict_put_raw(out, "w", w) }
   out
}

fn _vec_from_like(int n, any x) any {
   if(_is_vecdict(x)){
      def x0, x1 = _vec_at(x, 0, 0), _vec_at(x, 1, 0)
      def x2, x3 = _vec_at(x, 2, 0), _vec_at(x, 3, 0)
      return _vec_make(n, x0, x1, x2, x3)
   }
   if(_is_list(x) || _is_tuple(x)){
      def x0, x1 = _seq_at(x, 0, 0), _seq_at(x, 1, 0)
      def x2, x3 = _seq_at(x, 2, 0), _seq_at(x, 3, 0)
      return _vec_make(n, x0, x1, x2, x3)
   }
   0
}

fn Vector2(any x=0, any y=nil) any {
   "Builds a 2D vector from scalars, another vector, or a 2-item sequence."
   if(y == nil){
      def out = _vec_from_like(2, x)
      if(out){ return out }
      y = 0
   }
   _vec_make(2, x, y)
}

fn Vector3(any x=0, any y=nil, any z=nil) any {
   "Builds a 3D vector from scalars, another vector, or a 3-item sequence."
   if(y == nil && z == nil){
      def out = _vec_from_like(3, x)
      if(out){ return out }
   }
   if(y == nil){ y = 0 }
   if(z == nil){ z = 0 }
   _vec_make(3, x, y, z)
}

fn Vector4(any x=0, any y=nil, any z=nil, any w=nil) any {
   "Builds a 4D vector from scalars, another vector, or a 4-item sequence."
   if(y == nil && z == nil && w == nil){
      def out = _vec_from_like(4, x)
      if(out){ return out }
   }
   if(y == nil){ y = 0 }
   if(z == nil){ z = 0 }
   if(w == nil){ w = 0 }
   _vec_make(4, x, y, z, w)
}

fn vec2(any x=0, any y=nil) any {
   "Convenience alias for `Vector2(x, y)`."
   Vector2(x, y)
}

fn vec3(any x=0, any y=nil, any z=nil) any {
   "Convenience alias for `Vector3(x, y, z)`."
   Vector3(x, y, z)
}

fn vec4(any x=0, any y=nil, any z=nil, any w=nil) any {
   "Convenience alias for `Vector4(x, y, z, w)`."
   Vector4(x, y, z, w)
}

fn _vec_div(any a, any b) any {
   def n = (_vec_dim(a) < _vec_dim(b)) ? _vec_dim(a) : _vec_dim(b)
   if(n == 2){
      return _vec_make(
         2,
         __div(_as_float(_vec_at(a, 0)), _as_float(_vec_at(b, 0))),
         __div(_as_float(_vec_at(a, 1)), _as_float(_vec_at(b, 1)))
      )
   }
   if(n == 4){
      return _vec_make(
         4,
         __div(_as_float(_vec_at(a, 0)), _as_float(_vec_at(b, 0))),
         __div(_as_float(_vec_at(a, 1)), _as_float(_vec_at(b, 1))),
         __div(_as_float(_vec_at(a, 2)), _as_float(_vec_at(b, 2))),
         __div(_as_float(_vec_at(a, 3)), _as_float(_vec_at(b, 3)))
      )
   }
   _vec_make(
      3,
      __div(_as_float(_vec_at(a, 0)), _as_float(_vec_at(b, 0))),
      __div(_as_float(_vec_at(a, 1)), _as_float(_vec_at(b, 1))),
      __div(_as_float(_vec_at(a, 2)), _as_float(_vec_at(b, 2)))
   )
}

fn _vec_divs(any a, any s) any {
   def n = _vec_dim(a)
   if(n == 2){
      return _vec_make(
         2,
         __div(_as_float(_vec_at(a, 0)), _as_float(s)),
         __div(_as_float(_vec_at(a, 1)), _as_float(s))
      )
   }
   if(n == 4){
      return _vec_make(
         4,
         __div(_as_float(_vec_at(a, 0)), _as_float(s)),
         __div(_as_float(_vec_at(a, 1)), _as_float(s)),
         __div(_as_float(_vec_at(a, 2)), _as_float(s)),
         __div(_as_float(_vec_at(a, 3)), _as_float(s))
      )
   }
   _vec_make(
      3,
      __div(_as_float(_vec_at(a, 0)), _as_float(s)),
      __div(_as_float(_vec_at(a, 1)), _as_float(s)),
      __div(_as_float(_vec_at(a, 2)), _as_float(s))
   )
}

fn _vec_zip2(any a, any b, int op) any {
   def na, nb = _vec_dim(a), _vec_dim(b)
   def n = (na < nb) ? na : nb
   if(op == 3){ return _vec_div(a, b) }
   mut x, y, z, w = 0, 0, 0, 0
   if(op == 0){
      x, y = __add(_vec_at(a, 0), _vec_at(b, 0)), __add(_vec_at(a, 1), _vec_at(b, 1))
      if(n >= 3){ z = __add(_vec_at(a, 2), _vec_at(b, 2)) }
      if(n >= 4){ w = __add(_vec_at(a, 3), _vec_at(b, 3)) }
   } else {
      x, y = __sub(_vec_at(a, 0), _vec_at(b, 0)), __sub(_vec_at(a, 1), _vec_at(b, 1))
      if(n >= 3){ z = __sub(_vec_at(a, 2), _vec_at(b, 2)) }
      if(n >= 4){ w = __sub(_vec_at(a, 3), _vec_at(b, 3)) }
   }
   _vec_make(n, x, y, z, w)
}

fn _vec_scale(any a, any s, int op) any {
   def n = _vec_dim(a)
   if(op == 1){ return _vec_divs(a, s) }
   mut x, y, z, w = 0, 0, 0, 0
   if(op == 0){
      x, y = __mul(_vec_at(a, 0), s), __mul(_vec_at(a, 1), s)
      if(n >= 3){ z = __mul(_vec_at(a, 2), s) }
      if(n >= 4){ w = __mul(_vec_at(a, 3), s) }
   }
   _vec_make(n, x, y, z, w)
}

fn _vec_dot(any a, any b) any {
   def na, nb = _vec_dim(a), _vec_dim(b)
   def n = (na < nb) ? na : nb
   mut acc = 0
   mut i = 0
   while(i < n){
      acc = __add(acc, __mul(_vec_at(a, i), _vec_at(b, i)))
      i += 1
   }
   acc
}

fn _vec_eq(any a, any b) bool {
   def n = _vec_dim(a)
   if(n != _vec_dim(b)){ return false }
   mut i = 0
   while(i < n){
      if(!__eq(_vec_at(a, i), _vec_at(b, i))){ return false }
      i += 1
   }
   true
}

fn _vec_to_str(any v) str {
   def n = _vec_dim(v)
   mut s = "vec3("
   if(n == 2){ s = "vec2(" }
   if(n == 4){ s = "vec4(" }
   s = s + to_str(_vec_at(v, 0, 0))
   if(n >= 2){ s = s + ", " + to_str(_vec_at(v, 1, 0)) }
   if(n >= 3){ s = s + ", " + to_str(_vec_at(v, 2, 0)) }
   if(n >= 4){ s = s + ", " + to_str(_vec_at(v, 3, 0)) }
   s + ")"
}

fn _index_error(str op, any key, any size) any {
   __panic(op + " index out of range: index=" + to_str(key) + ", size=" + to_str(size))
   0
}

def _index_read_probe_on = false

@inline
fn _index_read_probe(any tag, any key, any path=0) int {
   if(_index_read_probe_on){ __index_read_probe(tag, key, path) }
   0
}

fn _index_read_type_error(any obj) any {
   __panic("index_read expects a string, bytes, list, tuple, dict, range, or vector, got " + type(obj) + " (" + repr(obj) + ")")
   0
}

fn _index_read_key_error(any key) any {
   __panic("index_read expects an integer index, got " + type(key) + " (" + repr(key) + ")")
   0
}

fn _index_read_oob_error(any key, any size) any {
   __panic("index_read out of range: index=" + to_str(key) + ", size=" + to_str(size))
   0
}

fn len(any x) int {
   "Returns the number of elements in a collection or the length of a string.
   - For **str**: number of bytes.
   - For **list/tuple/dict/set**: number of items.
   - For **bytes**: buffer size.
   Panics for unsupported types."
   if(__is_str_obj(x)){ return _raw_len(x) }
   if(_is_vecdict(x)){ return _vec_dim(x) }
   if(_is_list(x) || _is_tuple(x) || _is_dict(x) || _is_set(x)){ return __load64_idx(x, 0) }
   if(_is_range(x)){
      def start = __load64_idx(x, 0)
      def stop = __load64_idx(x, 8)
      def step = __load64_idx(x, 16)
      if(step == 0){ return 0 }
      if(step > 0){
         if(start >= stop){ return 0 }
         return((stop - start - 1) / step) + 1
      }
      if(start <= stop){ return 0 }
      return((start - stop - 1) / (0 - step)) + 1
   }
   if(_is_bytes(x)){ return _raw_len(x) }
   if(_is_float(x)){ return 0 }
   _type_error("len", "a string, bytes, list, tuple, dict, set, or range", x)
}

fn _range_contains(any container, any item) bool {
   def n = container.len
   if(n <= 0){ return false }
   def start = __load64_idx(container, 0)
   def step = __load64_idx(container, 16)
   if(step == 0){ return false }
   def delta = item - start
   if(step > 0){
      if(delta < 0){ return false }
   } else {
      if(delta > 0){ return false }
   }
   if(delta % step != 0){ return false }
   def idx = delta / step
   idx >= 0 && idx < n
}

fn contains(any container, any item) bool {
   "Returns **true** if `item` exists within `container`.
   - **set/dict**: checks for key existence.
   - **list/tuple**: checks for value presence.
   - **str**: checks for substring presence."
   if(_is_dict(container)){
      if(_is_vecdict(container)){
         mut i = 0
         def n = _vec_dim(container)
         while(i < n){
            if(eq(_vec_at(container, i), item)){ return true }
            i += 1
         }
         return false
      }
      return _dict_has_raw(container, item)
   }
   if(_is_set(container)){
      def cap = __load64_idx(container, 8)
      mut i = 0
      while(i < cap){
         def off = 16 + i * 24
         if(__load64_idx(container, off + 16) == 1){ if(eq(__load64_idx(container, off), item)){ return true } }
         i += 1
      }
      return false
   }
   if(_is_list(container) || _is_tuple(container)){
      mut i = 0
      def n = __load64_idx(container, 0)
      while(i < n){
         if(eq(__load_item(container, i), item)){ return true }
         i += 1
      }
      return false
   }
   if(_is_range(container)){
      return _range_contains(container, item)
   }
   if(__is_str_obj(container)){ return find(container, item) >= 0 }
   _type_error("contains", "a string, list, tuple, dict, set, range, or vector", container)
}

fn type(any x) str {
   "Returns a string representing the **tag-type** of Nytrix value `x`.
   Return values: `none`, `int`, `float`, `str`, `list`, `dict`, `set`,
   `tuple`, `bytes`, `bigint`, `bool`, `ptr`, `unknown`."
   if(__is_int(x)){ return "int" }
   if(__eq(x, true) || __eq(x, false)){ return "bool" }
   if(_is_float(x)){ return "float" }
   if(_is_bigint(x)){ return "bigint" }
   if(_is_str(x)){ return "str" }
   if(_is_list(x)){ return "list" }
   if(_is_dict(x)){ return "dict" }
   if(_is_set(x)){ return "set" }
   if(_is_tuple(x)){ return "tuple" }
   if(_is_range(x)){ return "range" }
   if(_is_bytes(x)){ return "bytes" }
   if(_has_tag(x, _TAG_COMPLEX)){ return "complex" }
   if(__eq(__tagof(x), _TAG_FFI_PTR)){ return "ffi_ptr" }
   if(is_ptr(x)){ return "ptr" }
   if(!x){ return "none" }
   return "unknown"
}

fn _type_shape_union_add(list shapes, str shape) int {
   mut i = 0
   while(i < shapes.len){
      if(shapes.get(i) == shape){ return 0 }
      i += 1
   }
   shapes.append(shape)
   1
}

fn _type_shape_union_from_seq(any xs, int depth) str {
   def n = xs.len
   if(n == 0){ return "empty" }
   mut shapes = list(n)
   mut i = 0
   while(i < n){
      _type_shape_union_add(shapes, _type_shape(xs.get(i), depth - 1))
      i += 1
   }
   join(shapes, "|")
}

fn _type_shape_tuple(any xs, int depth) str {
   def n = xs.len
   if(n == 0){ return "tuple<>" }
   mut shapes = list(n)
   mut i = 0
   while(i < n){
      shapes.append(_type_shape(xs.get(i), depth - 1))
      i += 1
   }
   "tuple<" + join(shapes, ", ") + ">"
}

fn _type_shape_dict(dict d, int depth) str {
   def its = items(d)
   def n = its.len
   if(n == 0){ return "dict<empty, empty>" }
   mut key_shapes = list(n)
   mut val_shapes = list(n)
   mut i = 0
   while(i < n){
      def pair = its.get(i)
      _type_shape_union_add(key_shapes, _type_shape(pair.get(0), depth - 1))
      _type_shape_union_add(val_shapes, _type_shape(pair.get(1), depth - 1))
      i += 1
   }
   "dict<" + join(key_shapes, "|") + ", " + join(val_shapes, "|") + ">"
}

fn _type_shape(any x, int depth) str {
   if(depth <= 0){ return type(x) }
   if(_is_vecdict(x)){ return _dict_get_raw(x, "__type", "dict") }
   if(_is_list(x)){ return "list<" + _type_shape_union_from_seq(x, depth) + ">" }
   if(_is_tuple(x)){ return _type_shape_tuple(x, depth) }
   if(_is_dict(x)){ return _type_shape_dict(x, depth) }
   if(_is_set(x)){ return "set<" + _type_shape_union_from_seq(items(x), depth) + ">" }
   type(x)
}

fn type_shape(any x, int max_depth=6) str {
   "Returns a recursive runtime type-shape string.
   `type(x)` intentionally returns only the top-level tag. Use this helper when
   debugging container contents, for example `list<list<int>>` or
   `dict<str, list<int>>`. Container shapes are inferred from current values,
   so empty containers are reported as `list<empty>`, `set<empty>`, or
   `dict<empty, empty>`."
   def depth = max_depth < 1 ? 1 : max_depth
   _type_shape(x, depth)
}

fn is_shape(any x, any spec, int max_depth=6) bool {
   "Returns true when `x` has the recursive runtime shape `spec`.
   `spec` can be a shape string or a list/tuple of acceptable shape strings."
   if(_is_list(spec) || _is_tuple(spec)){
      mut i = 0
      while(i < spec.len){
         if(is_shape(x, spec.get(i), max_depth)){ return true }
         i += 1
      }
      return false
   }
   if(!_is_str(spec)){ return false }
   type_shape(x, max_depth) == spec
}

fn _shape_spec_to_str(any spec) str {
   if(_is_list(spec) || _is_tuple(spec)){ return join(spec, "|") }
   to_str(spec)
}

fn require_shape(any x, any spec, str msg="shape check failed", int max_depth=6) any {
   "Returns `x` when it matches `spec`; otherwise panics with expected and actual shapes."
   if(!is_shape(x, spec, max_depth)){ __panic(msg + ": expected " + _shape_spec_to_str(spec) + ", got " + type_shape(x, max_depth)) }
   x
}

fn assert_shape(any x, any spec, str msg="shape check failed", int max_depth=6) any {
   "Alias for `require_shape`; useful when documenting shape contracts in runtime tests."
   require_shape(x, spec, msg, max_depth)
}

fn add(any a, any b) any {
   "Generic addition.
   - **string + string**: concatenation.
   - **list/tuple + list/tuple**: concatenation.
   - Other types: delegates to builtin `__add` (ints, floats, ptr math)."
   if(__is_int(a)){
      if(__is_int(b) || _is_float(b)){ return __add(a, b) }
   } elif(_is_float(a)){
      if(__is_int(b) || _is_float(b)){ return __add(a, b) }
   }
   if(_is_str(a) && _is_str(b)){ return __add(a, b) }
   if(_is_list_or_tuple(a) && _is_list_or_tuple(b)){ return __add(a, b) }
   if(_is_vecdict(a)){
      if(_is_vecdict(b)){ return _vec_zip2(a, b, 0) }
   }
   return __add(a, b)
}

fn sub(any a, any b) any {
   "Generic subtraction with list support.
   - **list/tuple - list/tuple**: element-wise difference(min length).
   - Other types: delegates to builtin `__sub`."
   if(__is_int(a)){
      if(__is_int(b) || _is_float(b)){ return __sub(a, b) }
   } elif(_is_float(a)){
      if(__is_int(b) || _is_float(b)){ return __sub(a, b) }
   }
   if(_is_list_or_tuple(a) && _is_list_or_tuple(b)){ return _list_zip2(a, b, 1) }
   if(_is_vecdict(a)){
      if(_is_vecdict(b)){ return _vec_zip2(a, b, 1) }
   }
   return __sub(a, b)
}

@returns_owned
fn _repeat_seq_like(any xs, int n) any {
   def orig_len = xs.len
   mut out = list(orig_len * n)
   mut rep = 0
   while(rep < n){
      mut j = 0
      while(j < orig_len){
         _store_item_raw(out, rep * orig_len + j, __load_item(xs, j))
         j += 1
      }
      rep += 1
   }
   _set_seq_count(out, orig_len * n)
   if(_is_tuple(xs)){ list_as_tuple_raw(out) }
   out
}

fn mul(any a, any b) any {
   "Generic multiplication.
   - **list/tuple * int**: sequence repetition.
   - **string * int**: string repetition.
   - **int * list/tuple**: sequence repetition(reversed).
   - **list/tuple * list/tuple**: element-wise product(min length).
   - **list/tuple * float**: scale each element.
   - Other types: delegates to builtin `__mul`."
   if(__is_int(a)){
      if(__is_int(b) || _is_float(b)){ return __mul(a, b) }
   } elif(_is_float(a)){
      if(__is_int(b) || _is_float(b)){ return __mul(a, b) }
   }
   if(__is_int(b) && b > 0){
      if(_is_str(a)){
         return repeat(a, b)
      }
      if(_is_list_or_tuple(a)){
         return _repeat_seq_like(a, b)
      }
   }
   if(__is_int(a) && a > 0){
      if(_is_list_or_tuple(b)){
         return _repeat_seq_like(b, a)
      }
   }
   if(_is_list_or_tuple(a)){
      if(_is_mat4(a)){
         if(_is_list_or_tuple(b) && b.len == 16){ return _mat4_mul(a, b) }
         if(_is_list_or_tuple(b) && b.len == 4){ return _mat4_mul_vec4(a, b) }
      }
      if(_is_float(b)){ return _list_scale(a, b, 0) }
      if(_is_list_or_tuple(b)){ return _list_zip2(a, b, 2) }
   }
   if(_is_vecdict(a)){
      if(_is_vecdict(b)){ return _vec_dot(a, b) }
      if(__is_int(b) || _is_float(b)){ return _vec_scale(a, b, 0) }
   }
   if(_is_vecdict(b)){ if(__is_int(a) || _is_float(a)){ return _vec_scale(b, a, 0) } }
   return __mul(a, b)
}

fn div(any a, any b) any {
   "Generic division.
   - **list/tuple / list/tuple**: element-wise division(min length).
   - **list/tuple / scalar**: divide each element by scalar.
   - Other types: delegates to builtin `__div`."
   if(_is_list_or_tuple(a) && _is_list_or_tuple(b)){ return _list_zip2(a, b, 3) }
   if(_is_list_or_tuple(a) && (__is_int(b) || _is_float(b))){ return _list_scale(a, b, 1) }
   if(_is_vecdict(a)){
      if(_is_vecdict(b)){ return _vec_zip2(a, b, 3) }
      if(__is_int(b) || _is_float(b)){ return _vec_scale(a, b, 1) }
   }
   return __div(a, b)
}

@inline
fn _is_list_or_tuple(any x) bool {
   def tag = __tagof(x)
   if(!__is_int(tag)){ return false }
   return __eq(tag, _TAG_LIST) || __eq(tag, _TAG_TUPLE)
}

@returns_owned
fn _list_like(int n) list { list(n) }

@returns_owned
fn _list_zip2(any a, any b, int op) any {
   def na, nb = a.len, b.len
   def n = (na < nb) ? na : nb
   def want_tuple = _is_tuple(a) && _is_tuple(b)
   mut out = _list_like(n)
   mut i = 0
   while(i < n){
      def x, y = __load_item(a, i), __load_item(b, i)
      def z = case op {
         0 -> __add(x, y)
         1 -> __sub(x, y)
         2 -> __mul(x, y)
         _ -> __div(x, y)
      }
      _store_item_raw(out, i, z)
      i += 1
   }
   __store64_idx(out, 0, n)
   if(want_tuple){ list_as_tuple_raw(out) }
   return out
}

@returns_owned
fn _list_scale(any a, any s, int op) any {
   def n = a.len
   def want_tuple = _is_tuple(a)
   mut out = _list_like(n)
   mut i = 0
   while(i < n){
      def x = __load_item(a, i)
      def z = case op {
         0 -> __mul(x, s)
         _ -> __div(x, s)
      }
      _store_item_raw(out, i, z)
      i += 1
   }
   __store64_idx(out, 0, n)
   if(want_tuple){ list_as_tuple_raw(out) }
   return out
}

fn _is_mat4(any x) bool { return _is_list_or_tuple(x) && x.len == 16 }

@returns_owned
fn _mat4_mul(any a, any b) list {
   mut out = list(16)
   mut r = 0
   while(r < 4){
      mut c = 0
      while(c < 4){
         mut s, k = 0, 0
         while(k < 4){
            s = s + a.get(r * 4 + k, 0) * b.get(k * 4 + c, 0)
            k += 1
         }
         out.append(s)
         c += 1
      }
      r += 1
   }
   return out
}

@returns_owned
fn _mat4_mul_vec4(any m, any v) list {
   mut out = list(4)
   mut r = 0
   while(r < 4){
      mut s, c = 0, 0
      while(c < 4){
         s = s + m.get(r * 4 + c, 0) * v.get(c, 0)
         c += 1
      }
      out.append(s)
      r += 1
   }
   return out
}

fn list_eq(any a, any b) bool {
   "Performs deep structural equality comparison for two lists."
   def na, nb = __load64_idx(a, 0), __load64_idx(b, 0)
   if(!(na == nb)){ return false }
   mut i = 0
   while(i < na){
      def va, vb = __load_item(a, i), __load_item(b, i)
      if(!eq(va, vb)){ return false }
      i += 1
   }
   return true
}

fn _seq_eq(any a, any b) bool {
   def na, nb = a.len, b.len
   if(!(na == nb)){ return false }
   mut i = 0
   while(i < na){
      if(!eq(a.get(i), b.get(i))){ return false }
      i += 1
   }
   return true
}

fn dict_eq(any a, any b) bool {
   "Performs deep structural equality comparison for two dictionaries."
   if(!(a.len == b.len)){ return false }
   def its = items(a)
   mut i = 0
   def n = its.len
   while(i < n){
      def p, k = its.get(i), p.get(0)
      if(!b.contains(k)){ return false }
      if(!eq(b.get(k, 0), p.get(1))){ return false }
      i += 1
   }
   return true
}

fn set_eq(any a, any b) bool {
   "Performs deep structural equality comparison for two sets."
   if(!(a.len == b.len)){ return false }
   def its = items(a)
   mut i = 0
   def n = its.len
   while(i < n){
      if(!(b.contains(its.get(i)))){ return false }
      i += 1
   }
   return true
}

fn eq(any a, any b) bool {
   "Structural equality operator. Compares values by content(strings/collections) or identity(primitives)."
   def same = __eq(a, b)
   if(same){ return true }
   def ta, tb = __tagof(a), __tagof(b)
   if(!__is_int(ta)){ return false }
   if(!__is_int(tb)){ return false }
   if(__eq(ta, _TAG_STR)){
      if(__eq(tb, _TAG_STR)){ return false }
      if(__eq(tb, _TAG_STR_CONST)){ return false }
      return false
   }
   if(__eq(ta, _TAG_STR_CONST)){
      if(__eq(tb, _TAG_STR)){ return false }
      if(__eq(tb, _TAG_STR_CONST)){ return false }
      return false
   }
   def a_seq, b_seq = _is_seq_tag(ta), _is_seq_tag(tb)
   def a_list_tuple, b_list_tuple = _is_list_tuple_tag(ta), _is_list_tuple_tag(tb)
   if(a_list_tuple){ if(b_list_tuple){ return false } }
   if(a_seq){ if(b_seq){ return _seq_eq(a, b) } }
   if(__eq(ta, _TAG_DICT)){
      if(__eq(tb, _TAG_DICT)){
         def av, bv = _is_vecdict(a), _is_vecdict(b)
         if(av){
            if(bv){ return _vec_eq(a, b) }
            return false
         }
         if(bv){ return false }
      }
   }
   if(__lt(ta, 100) || __gt(ta, 255) || __lt(tb, 100) || __gt(tb, 255)){ return false }
   if(__eq(ta, tb)){
      if(__eq(ta, _TAG_LIST) || __eq(ta, _TAG_TUPLE)){ return list_eq(a, b) }
      if(__eq(ta, _TAG_DICT)){ return dict_eq(a, b) }
      if(__eq(ta, _TAG_SET)){ return set_eq(a, b) }
      if(__eq(ta, _TAG_RANGE)){ return _seq_eq(a, b) }
      if(__eq(ta, _TAG_FLOAT)){ return __flt_eq(a, b) }
      if(__eq(ta, _TAG_BIGINT)){ return __eq(__bigint_cmp(a, b), 0) }
      return false
   } else {
      return false
   }
}

fn _repr_seq(any xs, str open, str close, int depth=0) str {
   def n = xs.len
   if(n == 0){ return open + close }
   mut parts = list(n * 2 + 1)
   _store_item_raw(parts, 0, open)
   mut i = 0
   mut pos = 1
   while(i < n){
      _store_item_raw(parts, pos, _repr_depth(xs.get(i), depth + 1))
      pos += 1
      if(i < n - 1){
         _store_item_raw(parts, pos, ", ")
         pos += 1
      }
      i += 1
   }
   _store_item_raw(parts, pos, close)
   _set_seq_count(parts, pos + 1)
   join(parts, "")
}

fn _repr_items(any its, bool pairs, str open="{", str close="}", int depth=0) str {
   def n = its.len
   if(n == 0){ return open + close }
   mut parts = list(n * 2 + 1)
   _store_item_raw(parts, 0, open)
   mut i = 0
   mut pos = 1
   while(i < n){
      if(pairs){
         def p = its.get(i)
         _store_item_raw(parts, pos, _repr_depth(p.get(0), depth + 1) + ": " + _repr_depth(p.get(1), depth + 1))
      } else {
         _store_item_raw(parts, pos, _repr_depth(its.get(i), depth + 1))
      }
      pos += 1
      if(i < n - 1){
         _store_item_raw(parts, pos, ", ")
         pos += 1
      }
      i += 1
   }
   _store_item_raw(parts, pos, close)
   _set_seq_count(parts, pos + 1)
   join(parts, "")
}

fn _repr_depth(any x, int depth) str {
   if(__is_int(x)){ return __to_str(x) }
   if(__eq(x, true)){ return "true" }
   if(__eq(x, false)){ return "false" }
   if(!__is_ptr(x) && !_is_str(x)){ return "none" }
   if(_is_str(x)){ return f"\"{x}\"" }
   if(!__is_ny_obj(x)){ return to_str(x) }
   def kind = __tagof(x)
   if(_is_bigint(x)){ return __bigint_to_str(x) }
   if(_is_vecdict(x)){ return _vec_to_str(x) }
   if(__eq(kind, _TAG_LIST)){
      if(depth >= 4){ return "[...]" }
      return _repr_seq(x, "[", "]", depth)
   }
   if(__eq(kind, _TAG_TUPLE)){
      if(depth >= 4){ return "(...)" }
      return _repr_seq(x, "(", ")", depth)
   }
   if(__eq(kind, _TAG_RANGE)){
      def start = __load64_idx(x, 0)
      def stop = __load64_idx(x, 8)
      def step = __load64_idx(x, 16)
      return f"range({start}, {stop}, {step})"
   }
   if(__eq(kind, _TAG_DICT)){
      if(depth >= 4){ return "{...}" }
      return _repr_items(items(x), true, "{", "}", depth)
   }
   if(__eq(kind, _TAG_SET)){
      if(depth >= 4){ return "{...}" }
      return _repr_items(items(x), false, "{", "}", depth)
   }
   if(__eq(kind, _TAG_FLOAT)){ return to_str(x) }
   if(__eq(kind, _TAG_COMPLEX)){ return __to_str(x) }
   if(__eq(kind, _TAG_BYTES)){ return f"<bytes {_raw_len(x)}>" }
   if(__eq(kind, _TAG_BIGINT)){ return __bigint_to_str(x) }
   f"<ptr {x} tag={__tagof(x)}>"
}

fn repr(any x) str {
   "Returns a programmer-friendly string representation of value `x`.
   - Strings are quoted.
   - Collections are shown with their structural contents and recursion guards.
   - Primitives are shown as their literal values."
   _repr_depth(x, 0)
}

fn hash(any x) int {
   "Returns a stable hash for primitive values. Currently supports integers, strings, and ranges."
   if(__is_int(x)){ return x }
   if(_is_range(x)){
      mut h = 14695981039346656037
      h = (h ^^ __load64_idx(x, 0)) * 1099511628211
      h = (h ^^ __load64_idx(x, 8)) * 1099511628211
      h = (h ^^ __load64_idx(x, 16)) * 1099511628211
      return h
   }
   if(_is_str(x)){ return __str_hash(x) }
   return 0
}

fn globals() any {
   "Returns a dictionary containing all currently defined global variables."
   return __globals()
}

@returns_owned
fn items(any x) list {
   "Generic item iterator. Returns a list of `[index/key, value]` pairs."
   if(_is_vecdict(x)){
      def n = _vec_dim(x)
      mut out = list(n)
      mut i = 0
      while(i < n){
         _store_item_raw(out, i, [i, _vec_at(x, i)])
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   if(_is_dict(x)){ return _dict_items_raw(x) }
   if(_is_set(x)){
      def cap = __load64_idx(x, 8)
      mut out = list(cap)
      mut i = 0
      while(i < cap){
         def off = 16 + i * 24
         if(__load64_idx(x, off + 16) == 1){ out.append(__load64_idx(x, off)) }
         i += 1
      }
      return out
   }
   if(_is_seq(x) || _is_str(x)){
      def n = x.len
      mut out = list(n)
      mut i = 0
      while(i < n){
         _store_item_raw(out, i, [i, x.get(i)])
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   return list(0)
}

@returns_owned
fn keys(any x) list {
   "Generic key iterator. Returns keys or indices for the given collection."
   if(_is_vecdict(x)){
      def n = _vec_dim(x)
      mut out = list(n)
      mut i = 0
      while(i < n){
         _store_item_raw(out, i, i)
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   if(_is_dict(x)){ return _dict_keys_raw(x) }
   if(_is_set(x)){ return items(x) }
   if(_is_seq(x) || _is_str(x)){
      def n = x.len
      mut out = list(n)
      mut i = 0
      while(i < n){
         _store_item_raw(out, i, i)
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   return list(0)
}

@returns_owned
fn values(any x) list {
   "Generic value iterator for all collection types."
   if(_is_vecdict(x)){
      def n = _vec_dim(x)
      mut out = list(n)
      mut i = 0
      while(i < n){
         _store_item_raw(out, i, _vec_at(x, i))
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   if(_is_dict(x)){ return _dict_values_raw(x) }
   if(_is_set(x)){ return items(x) }
   if(_is_range(x)){
      def n = x.len
      def step = __load64_idx(x, 16)
      mut cur = __load64_idx(x, 0)
      mut out = list(n)
      mut i = 0
      while(i < n){
         _store_item_raw(out, i, cur)
         cur = cur + step
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   if(_is_list(x) || _is_tuple(x) || _is_str(x)){
      def n = x.len
      mut out = list(n)
      mut i = 0
      while(i < n){
         _store_item_raw(out, i, x.get(i))
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   return list(0)
}

fn index_read(any obj, any key) any {
   "Strict indexed read used by `obj[key]` lowering."
   def tag = __tagof(obj)
   if(__eq(tag, _TAG_LIST) || __eq(tag, _TAG_TUPLE)){
      mut k = 0
      if(__is_int(key)){ k = key }
      elif(_is_bigint(key)){ k = __bigint_to_int(key) }
      else { return _index_read_key_error(key) }
      if(!__is_int(k)){ return _index_read_key_error(key) }
      def n = __load64_idx(obj, 0)
      if(__lt(k, 0)){ k = __add(k, n) }
      if(__lt(k, 0) || __ge(k, n)){ return _index_read_oob_error(k, n) }
      _index_read_probe(tag, k, 0)
      return __load_item(obj, k)
   }
   if(__eq(tag, _TAG_DICT)){
      if(_is_vecdict(obj)){
         mut k = 0
         if(__is_int(key)){ k = key }
         elif(_is_bigint(key)){ k = __bigint_to_int(key) }
         else { return _index_read_key_error(key) }
         if(!__is_int(k)){ return _index_read_key_error(key) }
         def n = _vec_dim(obj)
         if(__lt(k, 0)){ k = __add(k, n) }
         if(__lt(k, 0) || __ge(k, n)){ return _index_read_oob_error(k, n) }
         _index_read_probe(tag, k, 0)
         return _vec_at(obj, k, 0)
      }
      _index_read_probe(tag, key, 0)
      return _dict_get_raw(obj, key, 0)
   }
   if(__eq(tag, _TAG_STR) || __eq(tag, _TAG_STR_CONST) || __is_str_obj(obj)){
      mut k = 0
      if(__is_int(key)){ k = key }
      elif(_is_bigint(key)){ k = __bigint_to_int(key) }
      else { return _index_read_key_error(key) }
      if(!__is_int(k)){ return _index_read_key_error(key) }
      def n = obj.len
      if(__lt(k, 0)){ k = __add(k, n) }
      if(__lt(k, 0) || __ge(k, n)){ return _index_read_oob_error(k, n) }
      _index_read_probe(tag, k, 0)
      use std.core.str
      return chr(load8(obj, k))
   }
   if(__eq(tag, _TAG_BYTES) || _is_bytes(obj)){
      mut k = 0
      if(__is_int(key)){ k = key }
      elif(_is_bigint(key)){ k = __bigint_to_int(key) }
      else { return _index_read_key_error(key) }
      if(!__is_int(k)){ return _index_read_key_error(key) }
      def n = obj.len
      if(__lt(k, 0)){ k = __add(k, n) }
      if(__lt(k, 0) || __ge(k, n)){ return _index_read_oob_error(k, n) }
      _index_read_probe(tag, k, 0)
      return load8(obj, k)
   }
   if(__eq(tag, _TAG_RANGE)){
      mut k = 0
      if(__is_int(key)){ k = key }
      elif(_is_bigint(key)){ k = __bigint_to_int(key) }
      else { return _index_read_key_error(key) }
      if(!__is_int(k)){ return _index_read_key_error(key) }
      def n = obj.len
      if(__lt(k, 0)){ k = __add(k, n) }
      if(__lt(k, 0) || __ge(k, n)){ return _index_read_oob_error(k, n) }
      _index_read_probe(tag, k, 0)
      def start = __load64_idx(obj, 0)
      def step = __load64_idx(obj, 16)
      return start + k * step
   }
   if(obj == globals()){ return 0 }
   _index_read_type_error(obj)
}

fn get(any obj, any key, any default=0) any {
   "Generic element retriever. Handles indexing for strings, lists, dicts, tuples, ranges, and vectors.
   - `obj`: Collection(str, list, dict, tuple, range, vector)
   - `key`: Index or Key
   - `default`: Value to return if key/index not found(default 0).
   "
   def tag = __tagof(obj)
   if(__eq(tag, _TAG_LIST) || __eq(tag, _TAG_TUPLE)){
      mut k = 0
      if(__is_int(key)){ k = key }
      elif(_is_bigint(key)){ k = __bigint_to_int(key) }
      else { return default }
      if(!__is_int(k)){ return default }
      def n = __load64_idx(obj, 0)
      if(__lt(k, 0)){ k = __add(k, n) }
      if(__lt(k, 0) || __ge(k, n)){ return default }
      else { return __load_item(obj, k) }
   }
   if(__eq(tag, _TAG_DICT)){
      if(_is_vecdict(obj)){
         if(__is_int(key)){ return _vec_at(obj, key, default) }
         if(_is_bigint(key)){ return _vec_at(obj, __bigint_to_int(key), default) }
      }
      return _dict_get_raw(obj, key, default)
   }
   if(__eq(tag, _TAG_STR) || __eq(tag, _TAG_STR_CONST) || __is_str_obj(obj)){
      mut k = 0
      if(__is_int(key)){ k = key }
      elif(_is_bigint(key)){ k = __bigint_to_int(key) }
      else { return default }
      if(!__is_int(k)){ return default }
      def n = obj.len
      if(__lt(k, 0)){ k = __add(k, n) }
      if(__lt(k, 0) || __ge(k, n)){ return default }
      else {
         use std.core.str
         return chr(load8(obj, k))
      }
   }
   if(__eq(tag, _TAG_BYTES) || _is_bytes(obj)){
      mut k = 0
      if(__is_int(key)){ k = key }
      elif(_is_bigint(key)){ k = __bigint_to_int(key) }
      else { return default }
      if(!__is_int(k)){ return default }
      def n = obj.len
      if(__lt(k, 0)){ k = __add(k, n) }
      if(__lt(k, 0) || __ge(k, n)){ return default }
      else { return load8(obj, k) }
   }
   if(__eq(tag, _TAG_RANGE)){
      mut k = 0
      if(__is_int(key)){ k = key }
      elif(_is_bigint(key)){ k = __bigint_to_int(key) }
      else { return default }
      if(!__is_int(k)){ return default }
      def n = obj.len
      if(__lt(k, 0)){ k = __add(k, n) }
      if(__lt(k, 0) || __ge(k, n)){ return default }
      else {
         def start = __load64_idx(obj, 0)
         def step = __load64_idx(obj, 16)
         return start + k * step
      }
   }
   if(obj == globals()){ return default }
   if(!obj || _is_raw_ptr_like(obj)){ return default }
   _type_error("get", "a string, bytes, list, tuple, dict, range, or vector", obj)
}

fn _set_impl(any obj, any key, any val) any {
   if(!obj || _is_raw_ptr_like(obj)){ return 0 }
   if(_is_vecdict(obj)){
      if(__is_int(key)){ return _vec_set(obj, key, val) }
      if(_is_bigint(key)){ return _vec_set(obj, __bigint_to_int(key), val) }
   }
   if(_is_dict(obj)){ return _dict_put_raw(obj, key, val) }
   elif(_is_list(obj)){
      mut k = 0
      if(__is_int(key)){ k = key }
      elif(_is_bigint(key)){ k = __bigint_to_int(key) }
      else { _type_error("set", "an integer index", key) }
      if(!__is_int(k)){ _type_error("set", "an integer index", key) }
      def n = obj.len
      def cap = __load64_idx(obj, 8)
      if(__lt(k, 0)){ k = __add(k, n) }
      if(__lt(k, 0) || __ge(k, cap)){ _index_error("set", k, cap) }
      else {
         __store_item_fast(obj, k, val)
         if(__ge(k, n)){ _set_seq_count(obj, __add(k, 1)) }
         return obj
      }
   }
   elif(_is_bytes(obj)){
      mut k = 0
      if(__is_int(key)){ k = key }
      elif(_is_bigint(key)){ k = __bigint_to_int(key) }
      else { _type_error("set", "an integer index", key) }
      if(!__is_int(k)){ _type_error("set", "an integer index", key) }
      def n = obj.len
      if(__lt(k, 0)){ k = __add(k, n) }
      if(__lt(k, 0) || __ge(k, n)){ _index_error("set", k, n) }
      store8(obj, val, k)
      return obj
   }
   else { _type_error("set", "a bytes, list, dict, or vector", obj) }
}

fn set_idx(any obj, any key, any val) any {
   "Generic element setter. Supported for dicts, lists, bytes, and vectors. Returns the object or 0 on failure."
   _set_impl(obj, key, val)
}

fn set(any obj, any key, any val) any {
   "Generic setter for dicts, lists, bytes, and vectors. Returns the mutated object for method chaining."
   _set_impl(obj, key, val)
}

@returns_owned
fn slice(any obj, int start, int stop, int step=1) any {
   "Generic **slice** operation for strings and lists."
   if(_is_str(obj)){ return utf8_slice(obj, start, stop, step) }
   elif(_is_list(obj)){
      def n = obj.len
      if(start < 0){ start = n + start }
      if(stop < 0){ stop = n + stop }
      if(step > 0){
         if(start < 0){ start = 0 }
         if(stop > n){ stop = n }
         if(start >= stop){ return list(0) }
      } else {
         if(start >= n){ start = n - 1 }
         if(stop < prim.sub(0, 1)){ stop = prim.sub(0, 1) }
         if(start <= stop){ return list(0) }
      }
      mut out = list(n)
      mut i = start
      if(step > 0){
         while(i < stop){
            out.append(obj.get(i))
            i = i + step
         }
      } else {
         while(i > stop){
            out.append(obj.get(i))
            i = i + step
         }
      }
      return out
   }
   else { _type_error("slice", "a string or list", obj) }
}

fn append(any lst, any v) any {
   "Appends value `v` to the end of list `lst`. Returns the(possibly reallocated) list ptr."
   if(!_is_list(lst)){ _type_error("append", "a list", lst) }
   else { return __append(lst, v) }
}

fn pop(any lst) any {
   "Removes and returns the last element from list `lst`. Returns `0` if empty."
   if(!_is_list(lst)){ _type_error("pop", "a list", lst) }
   else {
      def n = __load64_idx(lst, 0)
      if(n == 0){ return 0 }
      else {
         def v = lst.get(n - 1)
         __store64_idx(lst, 0, n - 1)
         return v
      }
   }
}

@returns_owned
fn _extend_str_owned(str lst, any other) str {
   mut out = lst
   mut i = 0
   def n = other.len
   while(i < n){
      out = out + other.get(i)
      i += 1
   }
   return out
}

@returns_owned
fn _extend_list_realloc(any lst, any other, int ln, int on) list {
   mut out = list(ln + on)
   mut i = 0
   while(i < ln){
      _store_item_raw(out, i, lst.get(i))
      i += 1
   }
   i = 0
   while(i < on){
      _store_item_raw(out, ln + i, other.get(i))
      i += 1
   }
   _set_seq_count(out, ln + on)
   out
}

fn extend(any lst, any other) any {
   "Appends all elements from collection `other` to `lst`.
   - list target: appends items and returns a list
   - string target: concatenates items and returns a string"
   if(_is_str(lst)){ return _extend_str_owned(lst, other) }
   if(!_is_list(lst)){ return lst }
   def ln, on = lst.len, other.len
   def cap = __load64_idx(lst, 8)
   if(ln + on <= cap){
      mut i = 0
      while(i < on){
         _store_item_raw(lst, ln + i, other.get(i))
         i += 1
      }
      _set_seq_count(lst, ln + on)
      return lst
   }
   _extend_list_realloc(lst, other, ln, on)
}

fn _str_seq_depth(any xs, str open, str close, int depth) str {
   def n = xs.len
   if(n == 0){ return open + close }
   mut parts = list(n * 2 + 1)
   _store_item_raw(parts, 0, open)
   mut i = 0
   mut pos = 1
   while(i < n){
      _store_item_raw(parts, pos, _to_str_depth(xs.get(i), depth + 1))
      pos += 1
      if(i < n - 1){
         _store_item_raw(parts, pos, ", ")
         pos += 1
      }
      i += 1
   }
   _store_item_raw(parts, pos, close)
   _set_seq_count(parts, pos + 1)
   join(parts, "")
}

fn _str_items_depth(any its, bool pairs, int depth, str open="{", str close="}") str {
   def n = its.len
   if(n == 0){ return open + close }
   mut parts = list(n * 2 + 1)
   _store_item_raw(parts, 0, open)
   mut i = 0
   mut pos = 1
   while(i < n){
      if(pairs){
         def p = its.get(i)
         _store_item_raw(parts, pos,
         _to_str_depth(p.get(0), depth + 1) + ": " + _to_str_depth(p.get(1), depth + 1))
      } else {
         _store_item_raw(parts, pos, _to_str_depth(its.get(i), depth + 1))
      }
      pos += 1
      if(i < n - 1){
         _store_item_raw(parts, pos, ", ")
         pos += 1
      }
      i += 1
   }
   _store_item_raw(parts, pos, close)
   _set_seq_count(parts, pos + 1)
   join(parts, "")
}

fn _to_str_depth(any v, int depth) str {
   if(__eq(v, true)){ return "true" }
   if(__eq(v, false)){ return "false" }
   if(__is_int(v)){ return __to_str(v) }
   if(!v){ return "none" }
   if(_is_str(v)){ return v }
   def kind = __tagof(v)
   if(!__is_ny_obj(v)){ return __to_str(v) }
   if(_is_bigint(v)){ return __bigint_to_str(v) }
   if(_is_vecdict(v)){ return _vec_to_str(v) }
   if(__eq(kind, _TAG_LIST)){
      if(depth >= 4){ return "[...]" }
      return _str_seq_depth(v, "[", "]", depth)
   }
   if(__eq(kind, _TAG_TUPLE)){
      if(depth >= 4){ return "(...)" }
      return _str_seq_depth(v, "(", ")", depth)
   }
   if(__eq(kind, _TAG_RANGE)){
      def start = __load64_idx(v, 0)
      def stop = __load64_idx(v, 8)
      def step = __load64_idx(v, 16)
      return f"range({start}, {stop}, {step})"
   }
   if(__eq(kind, _TAG_DICT)){
      if(depth >= 4){ return "{...}" }
      return _str_items_depth(items(v), true, depth)
   }
   if(__eq(kind, _TAG_SET)){
      if(depth >= 4){ return "{...}" }
      return _str_items_depth(items(v), false, depth)
   }
   if(__eq(kind, _TAG_BYTES)){ return f"<bytes {_raw_len(v)}>" }
   if(__eq(kind, _TAG_FLOAT)){ return __to_str(v) }
   if(__eq(kind, _TAG_COMPLEX)){ return __to_str(v) }
   if(__eq(kind, _TAG_BIGINT)){ return __bigint_to_str(v) }
   f"<ptr {v} tag={__tagof(v)}>"
}

fn to_str(any v) str {
   "Returns a human-readable string representation of value `v`.
   - Strings are returned as-is.
   - Collections are shown with structural contents and recursion guards.
   - Ints/floats/bigints are converted to their decimal form."
   _to_str_depth(v, 0)
}

#main {
   fn _reflect_check(bool cond, str msg) int {
      if(!cond){ __panic(msg) }
      0
   }
   _reflect_check(type(42) == "int" && type("hello") == "str" && type([1, 2, 3]) == "list" && type(dict(8)) == "dict", "reflect types")
   _reflect_check(len([1, 2, 3]) == 3 && len("hello") == 5, "reflect len")
   _reflect_check(contains([1, 2, 3], 2) && contains("hello world", "world"), "reflect contains")
   _reflect_check(eq([1, 2, 3], [1, 2, 3]) && !eq([1, 2, 3], [1, 2, 4]), "reflect list equality")
   mut d1 = dict(8)
   d1["a"] = 1
   d1["b"] = 2
   mut d2 = dict(8)
   d2["a"] = 1
   d2["b"] = 2
   _reflect_check(eq(d1, d2), "reflect dict equality")
   _reflect_check(to_str([1, 2, 3]) == "[1, 2, 3]", "reflect list to_str")
   _reflect_check(repr("hello") == "\"hello\"", "reflect string repr")
   _reflect_check(repr(["a", "b"]) == "[\"a\", \"b\"]", "reflect list repr")
   _reflect_check(hash("hello") == hash("hello") && hash("hello") != hash("world"), "reflect hash")
   def deep_list = [[[[[1]]]]]
   _reflect_check(repr(deep_list).contains("[...]") && to_str(deep_list).contains("[...]"), "reflect deep list rendering")
   def deep_dict = {"a": {"b": {"c": {"d": {"e": 1}}}}}
   _reflect_check(repr(deep_dict).contains("{...}") && to_str(deep_dict).contains("{...}"), "reflect deep dict rendering")
   def writable = [1, 2, 3]
   writable[1] = 9
   _reflect_check(writable.get(1) == 9, "reflect set_idx raw store")
   def v2 = Vector2([3, 4])
   def v3 = Vector3([1, 2, 3])
   def v4 = Vector4([5, 6, 7, 8])
   _reflect_check(len(v2) == 2 && len(v3) == 3 && len(v4) == 4, "reflect vector dimensions")
   _reflect_check(get(v2, "__type", "") == "vec2" && get(v3, "__type", "") == "vec3" && get(v4, "__type", "") == "vec4", "reflect vector type markers")
   _reflect_check(contains((1, 2, 3), 3), "reflect tuple contains")
   def pair_product = mul([2, 3], [4, 5])
   def pair_quotient = div([8, 9], [2, 3])
   _reflect_check(pair_product[0] == 8 && pair_product[1] == 15, "reflect list pair multiplication")
   _reflect_check(pair_quotient[0] == 4 && pair_quotient[1] == 3, "reflect list pair division")
   def repeat_result = mul((6, 7), 2)
   _reflect_check(repeat_result.len == 4 && repeat_result[2] == 6 && repeat_result[3] == 7, "reflect tuple repeat")
   print("✓ std.core.reflect self-test passed")
}
