;; Keywords: reflect reflection introspection core
;; Runtime value reflection and compile-time immutable compiler fact snapshots.
;; References:
;; - core
module core.reflect(len, contains, type, type_shape, is_shape, require_shape,
   assert_shape, add, sub, mul, div, list_eq, dict_eq,
   set_eq, eq, repr, hash, globals, items, keys, values,
   get, index_read, set, set_idx, slice, append, pop,
   extend, to_str, vec2, vec3, vec4, Vector2, Vector3,
   Vector4, layout_for, fields_for, fields_with_origins_for,
   exports_for, variants_for, variants_with_origins_for,
   attributes_for, semantic_snapshot, semantic_get,
   semantic_items, semantic_functions,
   semantic_find_function, semantic_layouts,
   semantic_find_function_at,
   semantic_find_layout, semantic_expressions,
   semantic_find_expression, semantic_calls,
   semantic_find_call, semantic_call_effects,
   semantic_call_effect_row,
   semantic_obligations, semantic_find_obligation,
   semantic_callable_effects,
   semantic_find_callable_effect,
   semantic_callable_effect_row,
   semantic_proof_obligations, semantic_find_invariant,
   _reflect_tuple,
   semantic_find_law,
   semantic_statement_effects,
   semantic_find_statement_effect, semantic_signature,
   semantic_statement_custom_effect_row,
   semantic_diagnostics,
   semantic_find_diagnostic,
   semantic_parameters, semantic_return,
   semantic_signature_effects, semantic_expression_type,
   semantic_expression_representation,
   semantic_expression_ownership,
   semantic_expression_mutable,
   semantic_expression_alias_class,
   semantic_expression_effects,
   semantic_expression_custom_effect_row)
use core.error
use core.str
use core.str as core_str
use core.primitives
use core.primitives as core_prim
use core.dict_mod
use core.set_mod as core_set
use core.compiler.syntax.type
use core.compiler.reflect.semantic as semantic_facts

;; Typed compiler artifacts use the same public reflection namespace as the
;; compile-time declaration snapshots below. The implementation remains in a
;; small dedicated reader module so the general runtime reflection library
;; does not own JSON parsing details.
fn semantic_snapshot(str source) dict {
   semantic_facts.semantic_snapshot(source)
}

fn semantic_get(any record, str key, any fallback=nil) any {
   semantic_facts.semantic_get(record, key, fallback)
}

fn semantic_items(any sequence) tuple {
   semantic_facts.semantic_items(sequence)
}

fn semantic_functions(any snapshot) tuple {
   semantic_facts.semantic_functions(snapshot)
}

fn semantic_find_function(any snapshot, str name, str owner="") any {
   semantic_facts.semantic_find_function(snapshot, name, owner)
}

fn semantic_find_function_at(any snapshot, str name, int line,
   int column=0, str file="") any {
   semantic_facts.semantic_find_function_at(snapshot, name, line, column,
      file)
}

fn semantic_layouts(any snapshot) tuple {
   semantic_facts.semantic_layouts(snapshot)
}

fn semantic_find_layout(any snapshot, str name, str owner="") any {
   semantic_facts.semantic_find_layout(snapshot, name, owner)
}

fn semantic_expressions(any function_record) tuple {
   semantic_facts.semantic_expressions(function_record)
}

fn semantic_find_expression(any function_record, str kind, int line,
   int column=0, str file="") any {
   semantic_facts.semantic_find_expression(function_record, kind, line,
      column, file)
}

fn semantic_calls(any function_record) tuple {
   semantic_facts.semantic_calls(function_record)
}

fn semantic_find_call(any function_record, int line, int column=0,
   str file="") any {
   semantic_facts.semantic_find_call(function_record, line, column, file)
}

fn semantic_call_effects(any call) any {
   semantic_facts.semantic_call_effects(call)
}

fn semantic_call_effect_row(any call) any {
   semantic_facts.semantic_call_effect_row(call)
}

fn semantic_obligations(any function_record) tuple {
   semantic_facts.semantic_obligations(function_record)
}

fn semantic_find_obligation(any function_record, str kind, int line,
   int column=0, str file="") any {
   semantic_facts.semantic_find_obligation(function_record, kind, line,
      column, file)
}

fn semantic_callable_effects(any snapshot) tuple {
   semantic_facts.semantic_callable_effects(snapshot)
}

fn semantic_find_callable_effect(any snapshot, str name, int arity=0) any {
   semantic_facts.semantic_find_callable_effect(snapshot, name, arity)
}

fn semantic_callable_effect_row(any callable) any {
   semantic_facts.semantic_callable_effect_row(callable)
}

fn semantic_proof_obligations(any snapshot) tuple {
   semantic_facts.semantic_proof_obligations(snapshot)
}

fn semantic_find_invariant(any snapshot, int line, int column=0,
   str file="") any {
   semantic_facts.semantic_find_invariant(snapshot, line, column, file)
}

fn semantic_find_law(any snapshot, str trait_name, str type_name,
   int line=0) any {
   semantic_facts.semantic_find_law(snapshot, trait_name, type_name, line)
}

fn semantic_statement_effects(any function_record) tuple {
   semantic_facts.semantic_statement_effects(function_record)
}

fn semantic_find_statement_effect(any function_record, str kind, int line,
   int column=0, str file="") any {
   semantic_facts.semantic_find_statement_effect(function_record, kind, line,
      column, file)
}

fn semantic_diagnostics(any snapshot) tuple {
   semantic_facts.semantic_diagnostics(snapshot)
}

fn semantic_find_diagnostic(any snapshot, str code, str file="",
   int line=0, int column=0) any {
   semantic_facts.semantic_find_diagnostic(snapshot, code, file, line, column)
}

fn semantic_statement_custom_effect_row(any statement) any {
   semantic_facts.semantic_statement_custom_effect_row(statement)
}

fn semantic_signature(any function_record) any {
   semantic_facts.semantic_signature(function_record)
}

fn semantic_parameters(any function_record) tuple {
   semantic_facts.semantic_parameters(function_record)
}

fn semantic_return(any function_record) any {
   semantic_facts.semantic_return(function_record)
}

fn semantic_signature_effects(any function_record) any {
   semantic_facts.semantic_signature_effects(function_record)
}

fn semantic_expression_type(any expression) any {
   semantic_facts.semantic_expression_type(expression)
}

fn semantic_expression_representation(any expression) any {
   semantic_facts.semantic_expression_representation(expression)
}

fn semantic_expression_ownership(any expression) any {
   semantic_facts.semantic_expression_ownership(expression)
}

fn semantic_expression_mutable(any expression) any {
   semantic_facts.semantic_expression_mutable(expression)
}

fn semantic_expression_alias_class(any expression) any {
   semantic_facts.semantic_expression_alias_class(expression)
}

fn semantic_expression_effects(any expression) any {
   semantic_facts.semantic_expression_effects(expression)
}

fn semantic_expression_custom_effect_row(any expression) any {
   semantic_facts.semantic_expression_custom_effect_row(expression)
}

;; Generate deeply immutable, versioned snapshots from compiler facts. Field
;; rows are tuples in this order: name, type, index, offset, explicit align,
;; array flag, known extent, extent, default flag, default source.
fn _reflect_tuple(list values) tuple {
   core_prim.list_as_tuple_raw(values)
}

comptime template fields_for(Layout) {
   fn ${Layout}_reflection_fields() tuple {
      mut result = []
      comptime fields(Layout) as field {
         emit result = result.append(_reflect_tuple([field.name, field.type,
                  field.index, field.offset, field.align, field.is_array,
                  field.array_len_known, field.array_len, field.has_default,
                  field.default_src]))
      }
      _reflect_tuple(["nytrix.reflect.fields", 1, _reflect_tuple(result)])
   }
}

;; Origin-aware rows extend the fields_for row with source file, line, and
;; column after the field name. Kept as a separate schema to preserve v1.
comptime template fields_with_origins_for(Layout) {
   fn ${Layout}_reflection_fields_with_origins() tuple {
      mut result = []
      comptime fields(Layout) as field {
         emit result = result.append(_reflect_tuple([field.name,
                  field.source_file, field.source_line, field.source_column,
                  field.type, field.index, field.offset, field.align, field.is_array,
                  field.array_len_known, field.array_len, field.has_default,
                  field.default_src]))
      }
      _reflect_tuple(["nytrix.reflect.fields.origins", 1,
            _reflect_tuple(result)])
   }
}

;; Aggregate ABI facts accompany the per-field snapshot. The compiler resolves
;; size and alignment using the same layout rules as allocation and FFI.
comptime template layout_for(Layout) {
   fn ${Layout}_reflection_layout() tuple {
      mut result = []
      comptime fields(Layout) as field {
         emit result = result.append(_reflect_tuple([field.name, field.type,
                  field.index, field.offset, field.align, field.is_array,
                  field.array_len_known, field.array_len, field.has_default,
                  field.default_src]))
      }
      _reflect_tuple(["nytrix.reflect.layout", 1, "${Layout}",
            __layout_size("${Layout}"), __layout_align("${Layout}"),
            _reflect_tuple(result)])
   }
}

comptime template exports_for(Module) {
   fn ${Module}_reflection_exports() tuple {
      mut result = []
      comptime exports(Module) as name {
         emit result = result.append(name)
      }
      _reflect_tuple(["nytrix.reflect.exports", 1, _reflect_tuple(result)])
   }
}

;; Variant rows are tuples in this order: name, tag index, payload flag,
;; payload count, and an immutable tuple of (field name, field type) pairs.
comptime template variants_for(Enum) {
   fn ${Enum}_reflection_variants() tuple {
      mut result = []
      comptime variants(Enum) as variant {
         emit result = result.append(_reflect_tuple([variant.name, variant.index,
                  variant.has_payload, variant.payload_count,
                  variant.payload_fields]))
      }
      _reflect_tuple(["nytrix.reflect.variants", 1, _reflect_tuple(result)])
   }
}

;; Origin-aware rows extend each variant row with source file, line, and column
;; after its name. Kept as a separate schema to preserve variants_for v1.
comptime template variants_with_origins_for(Enum) {
   fn ${Enum}_reflection_variants_with_origins() tuple {
      mut result = []
      comptime variants(Enum) as variant {
         emit result = result.append(_reflect_tuple([variant.name,
                  variant.source_file, variant.source_line, variant.source_column,
                  variant.index, variant.has_payload, variant.payload_count,
                  variant.payload_fields_with_origins]))
      }
      _reflect_tuple(["nytrix.reflect.variants.origins", 1,
            _reflect_tuple(result)])
   }
}

;; Attribute names are returned in source order as an immutable tuple.
comptime template attributes_for(Declaration) {
   fn ${Declaration}_reflection_attributes() tuple {
      mut result = []
      comptime attributes(Declaration) as attribute {
         emit result = result.append(attribute)
      }
      _reflect_tuple(["nytrix.reflect.attributes", 1,
            _reflect_tuple(result)])
   }
}

@inline
fn _has_tag(any x, int tag) bool {
   return __tagof(x) == tag
}

def _TAG_DICT = runtime_tag_raw("dict")
def _TAG_LIST = runtime_tag_raw("list")
def _TAG_SET = runtime_tag_raw("set")
def _TAG_TUPLE = runtime_tag_raw("tuple")
def _TAG_RANGE = runtime_tag_raw("range")
def _TAG_BYTES = runtime_tag_raw("bytes")
def _TAG_STR = runtime_tag_raw("str")
def _TAG_STR_CONST = runtime_tag_raw("str_const")
def _TAG_PTR = runtime_tag_raw("ptr")
def _TAG_FFI_PTR = runtime_tag_raw("ffi_ptr")
def _TAG_BIGINT = runtime_tag_raw("bigint")
def _TAG_BIGFLOAT = runtime_tag_raw("bigfloat")
def _TAG_FLOAT = runtime_tag_raw("float")
def _TAG_COMPLEX = runtime_tag_raw("complex")

@inline
fn _is_dict(any x) bool {
   ;; Native dictionary handles do not expose the same immediate tag word as
   ;; legacy boxed dictionaries.  Use the runtime predicate, which understands
   ;; both representations, so recursive shape inspection doesn't stop at the
   ;; top-level `dict` tag for native/heterogeneous dictionaries.
   return is_dict(x)
}

@inline
fn _is_list(any x) bool { return _has_tag(x, _TAG_LIST) }

@inline
fn _is_set(any x) bool { return _has_tag(x, _TAG_SET) }

@inline
fn _is_tuple(any x) bool { return _has_tag(x, _TAG_TUPLE) }

@inline
fn _is_range(any x) bool { return _has_tag(x, _TAG_RANGE) }

@inline
fn _is_bytes(any x) bool { return _has_tag(x, _TAG_BYTES) }

@inline
fn _is_raw_ptr_like(any x) bool {
   if __is_int(x) || !x { return false }
   def tag = __tagof(x)
   def seq = _is_seq_tag(tag)
   if tag == _TAG_PTR || tag == _TAG_FFI_PTR { return true }
   if tag == 0 && core_prim.is_ptr(x) { return true }
   if seq { return false }
   if _is_str_tag(tag) { return false }
   if _has_tag(x, _TAG_DICT) { return false }
   if _has_tag(x, _TAG_SET) { return false }
   if _has_tag(x, _TAG_BYTES) { return false }
   if _is_bigint(x) { return false }
   if _is_bigfloat(x) { return false }
   return core_prim.is_ptr(x) != 0
}

@inline
fn _is_seq_tag(int tag) bool {
   return (tag == _TAG_LIST) || (tag == _TAG_TUPLE) || (tag == _TAG_RANGE)
}

@inline
fn _is_list_tuple_tag(int tag) bool {
   return (tag == _TAG_LIST) || (tag == _TAG_TUPLE)
}

@inline
fn _is_str_tag(int tag) bool {
   return (tag == _TAG_STR) || (tag == _TAG_STR_CONST)
}

@inline
fn _is_seq(any x) bool { return _is_seq_tag(__tagof(x)) }

@inline
fn _is_bigint(any x) bool { return _has_tag(x, _TAG_BIGINT) }

@inline
fn _is_bigfloat(any x) bool { return _has_tag(x, _TAG_BIGFLOAT) }

@inline
fn _is_str(any x) bool {
   ;; __tagof returns a raw runtime tag; routing it through the generic
   ;; _is_str_tag(any) helper makes the legacy REPL/JIT treat that raw tag as
   ;; a language value.  Ask the representation-aware runtime predicate
   ;; directly instead (it also distinguishes native tbuf containers safely).
   return __is_str_obj(x)
}

@inline
fn _is_float(any x) bool { return __is_float_obj(x) }

@inline
fn _dict_get_raw(any d, any key, any default=0) any { return dict_get(d, key, default) }

@inline
fn _dict_put_raw(any d, any key, any value) any { return dict_set(d, key, value) }

@inline
fn _dict_has_raw(any d, any key) bool { return dict_exists(d, key) }

@inline
fn _dict_items_raw(any d) list { return dict_items(d) }

@inline
fn _dict_keys_raw(any d) list { return dict_keys(d) }

@inline
fn _dict_values_raw(any d) list { return dict_values(d) }

@inline
fn _set_seq_count(any xs, int count) any {
   __list_set_len(xs, count)
   return xs
}

@inline
fn _raw_len(any obj) int { return __str_len(obj) }

@inline
fn _float_one() any { return __flt_box_val(__flt_from_int(1)) }

@inline
fn _as_float(any x) any { return __mul(x, _float_one()) }

@inline
fn _store_item_raw(list xs, int index, any value) list {
   __store_item_fast(xs, index, value)
   return xs
}

fn _type_error(str op, str want, any got) any {
   __panic(op + " expects " + want + ", got " + type(got) + " (" + repr(got) + ")")
   0
}

fn _vec_dim_type(any t) int {
   return case t {
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
   if !_is_dict(x) { return false }
   return _vec_dim_type(_dict_get_raw(x, "__type", "")) > 0
}

fn _vec_dim(any x) int { return _vec_dim_type(_dict_get_raw(x, "__type", "")) }

fn _seq_at(any x, int i, any default=0) any {
   def n = __load64_idx(x, 0)
   if i < 0 || i >= n { return default }
   return __load_item(x, i)
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
   if n >= 3 { out = _dict_put_raw(out, "z", z) }
   if n >= 4 { out = _dict_put_raw(out, "w", w) }
   out
}

fn _vec_from_like(int n, any x) any {
   if _is_vecdict(x) {
      def x0, x1 = _vec_at(x, 0, 0), _vec_at(x, 1, 0)
      def x2, x3 = _vec_at(x, 2, 0), _vec_at(x, 3, 0)
      return _vec_make(n, x0, x1, x2, x3)
   }
   if _is_list(x) || _is_tuple(x) {
      def x0, x1 = _seq_at(x, 0, 0), _seq_at(x, 1, 0)
      def x2, x3 = _seq_at(x, 2, 0), _seq_at(x, 3, 0)
      return _vec_make(n, x0, x1, x2, x3)
   }
   0
}

fn Vector2(any x=0, any y=nil) any {
   "Builds a 2D vector from scalars, another vector, or a 2-item sequence."
   if y == nil {
      def out = _vec_from_like(2, x)
      if out { return out }
      y = 0
   }
   _vec_make(2, x, y)
}

fn Vector3(any x=0, any y=nil, any z=nil) any {
   "Builds a 3D vector from scalars, another vector, or a 3-item sequence."
   if y == nil && z == nil {
      def out = _vec_from_like(3, x)
      if out { return out }
   }
   if y == nil { y = 0 }
   if z == nil { z = 0 }
   _vec_make(3, x, y, z)
}

fn Vector4(any x=0, any y=nil, any z=nil, any w=nil) any {
   "Builds a 4D vector from scalars, another vector, or a 4-item sequence."
   if y == nil && z == nil && w == nil {
      def out = _vec_from_like(4, x)
      if out { return out }
   }
   if y == nil { y = 0 }
   if z == nil { z = 0 }
   if w == nil { w = 0 }
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
   if n == 2 {
      return _vec_make(
         2,
         __div(_as_float(_vec_at(a, 0)), _as_float(_vec_at(b, 0))),
         __div(_as_float(_vec_at(a, 1)), _as_float(_vec_at(b, 1)))
      )
   }
   if n == 4 {
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
   if n == 2 {
      return _vec_make(
         2,
         __div(_as_float(_vec_at(a, 0)), _as_float(s)),
         __div(_as_float(_vec_at(a, 1)), _as_float(s))
      )
   }
   if n == 4 {
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
   def int n = (na < nb) ? na : nb
   if op == 3 { return _vec_div(a, b) }
   ; These components are intentionally dynamic: overloaded arithmetic can
   ; produce scalars or vector-like values, and _vec_make stores them as any.
   ; Keeping their locals tagged prevents branch joins from assigning one
   ; aggregate class to every component slot.
   mut any x, y, z, w = 0, 0, 0, 0
   if op == 0 {
      x, y = __add(_vec_at(a, 0), _vec_at(b, 0)), __add(_vec_at(a, 1), _vec_at(b, 1))
      if n >= 3 { z = __add(_vec_at(a, 2), _vec_at(b, 2)) }
      if n >= 4 { w = __add(_vec_at(a, 3), _vec_at(b, 3)) }
   } else {
      x, y = __sub(_vec_at(a, 0), _vec_at(b, 0)), __sub(_vec_at(a, 1), _vec_at(b, 1))
      if n >= 3 { z = __sub(_vec_at(a, 2), _vec_at(b, 2)) }
      if n >= 4 { w = __sub(_vec_at(a, 3), _vec_at(b, 3)) }
   }
   _vec_make(n, x, y, z, w)
}

fn _vec_scale(any a, any s, int op) any {
   def n = _vec_dim(a)
   if op == 1 { return _vec_divs(a, s) }
   mut x, y, z, w = 0, 0, 0, 0
   if op == 0 {
      x, y = __mul(_vec_at(a, 0), s), __mul(_vec_at(a, 1), s)
      if n >= 3 { z = __mul(_vec_at(a, 2), s) }
      if n >= 4 { w = __mul(_vec_at(a, 3), s) }
   }
   _vec_make(n, x, y, z, w)
}

fn _vec_dot(any a, any b) any {
   def na, nb = _vec_dim(a), _vec_dim(b)
   def n = (na < nb) ? na : nb
   mut acc = 0
   mut i = 0
   while i < n {
      acc = __add(acc, __mul(_vec_at(a, i), _vec_at(b, i)))
      i += 1
   }
   acc
}

fn _vec_eq(any a, any b) bool {
   def n = _vec_dim(a)
   if n != _vec_dim(b) { return false }
   mut i = 0
   while i < n {
      if !(_vec_at(a, i) == _vec_at(b, i)) { return false }
      i += 1
   }
   true
}

fn _vec_to_str(any v) str {
   def int n = _vec_dim(v)
   mut str s = "vec3("
   if n == 2 { s = "vec2(" }
   if n == 4 { s = "vec4(" }
   ;; `_vec_at` is deliberately dynamic. Bind each component through its
   ;; declared `any` carrier before formatting so the native path does not
   ;; mistake an inferred numeric use for an f64 register at a later join.
   def any c0 = _vec_at(v, 0, 0)
   s = s + to_str(c0)
   if n >= 2 {
      def any c1 = _vec_at(v, 1, 0)
      s = s + ", " + to_str(c1)
   }
   if n >= 3 {
      def any c2 = _vec_at(v, 2, 0)
      s = s + ", " + to_str(c2)
   }
   if n >= 4 {
      def any c3 = _vec_at(v, 3, 0)
      s = s + ", " + to_str(c3)
   }
   s + ")"
}

fn _index_error(str op, any key, any size) any {
   __panic(op + " index out of range: index=" + to_str(key) + ", size=" + to_str(size))
   0
}

def _index_read_probe_on = false

@inline
fn _index_read_probe(any tag, any key, any path=0) int {
   if _index_read_probe_on { __index_read_probe(tag, key, path) }
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
   ;; Lists/tuples may use either the legacy managed header or the native
   ;; tbuf layout.  __list_len handles both; reading byte offset zero directly
   ;; reads the first element of a native tbuf instead of its count.
   if _is_list(x) || _is_tuple(x) { return __list_len(x) }
   if _is_dict(x) {
      if _is_vecdict(x) { return _vec_dim(x) }
      return dict_len(x)
   }
   if _is_set(x) { return core_set.len(x) }
   if _is_str(x) { return __str_len(x) }
   if _is_bytes(x) { return __load64_idx(x, -16) >> 1 }
   if _is_range(x) {
      def start = __load64_idx(x, 0)
      def stop = __load64_idx(x, 8)
      def step = __load64_idx(x, 16)
      if step > 0 && start < stop { return (stop - start + step - 1) / step }
      if step < 0 && start > stop { return (start - stop - step - 1) / (-step) }
      return 0
   }
   __panic("len expects sequence or collection, got " + type(x))
   0
}

fn _range_contains(any container, any item) bool {
   def int n = container.len
   if n <= 0 { return false }
   def start = __load64_idx(container, 0)
   def step = __load64_idx(container, 16)
   if step == 0 { return false }
   def delta = item - start
   if step > 0 {
      if delta < 0 { return false }
   } else {
      if delta > 0 { return false }
   }
   if delta % step != 0 { return false }
   def idx = delta / step
   idx >= 0 && idx < n
}

fn contains(any container, any item) bool {
   "Returns **true** if `item` exists within `container`.
   - **set/dict**: checks for key existence.
   - **list/tuple**: checks for value presence.
   - **str**: checks for substring presence."
   if _is_dict(container) {
      if _is_vecdict(container) {
         mut i = 0
         def n = _vec_dim(container)
         while i < n {
            if eq(_vec_at(container, i), item) { return true }
            i += 1
         }
         return false
      }
      return _dict_has_raw(container, item)
   }
   if _is_set(container) {
      def cap = __load64_idx(container, 8)
      mut i = 0
      while i < cap {
         def off = 16 + i * 24
         if __load64_idx(container, off + 16) == 1 { if eq(__load64_idx(container, off), item) { return true } }
         i += 1
      }
      return false
   }
   if _is_list(container) || _is_tuple(container) {
      mut i = 0
      def n = __list_len(container)
      while i < n {
         if eq(__load_item(container, i), item) { return true }
         i += 1
      }
      return false
   }
   if _is_range(container) {
      return _range_contains(container, item)
   }
   if __is_str_obj(container) { return find(container, item) >= 0 }
   _type_error("contains", "a string, list, tuple, dict, set, range, or vector", container)
}

fn type(any x) str {
   "Returns a string representing the **tag-type** of Nytrix value `x`.
   Return values: `nil`, `int`, `float`, `str`, `list`, `dict`, `set`,
   `tuple`, `bytes`, `bigint`, `bool`, `ptr`, `unknown`."
   if core_prim.is_nil(x) { return "nil" }
   if __is_int(x) { return "int" }
   if x == true || x == false { return "bool" }
   if _is_float(x) { return "float" }
   if _is_bigint(x) { return "bigint" }
   if _is_bigfloat(x) { return "bigfloat" }
   if _is_str(x) { return "str" }
   if _is_list(x) { return "list" }
   if _is_dict(x) { return "dict" }
   if _is_set(x) { return "set" }
   if _is_tuple(x) { return "tuple" }
   if _is_range(x) { return "range" }
   if _is_bytes(x) { return "bytes" }
   if _has_tag(x, _TAG_COMPLEX) { return "complex" }
   if __tagof(x) == _TAG_FFI_PTR { return "ffi_ptr" }
   if is_ptr(x) { return "ptr" }
   return "unknown"
}

fn _type_shape_union_add(list shapes, str shape) list {
   ;; Dynamic list storage may expose an uninitialized slot while a literal is
   ;; being inspected.  Do not widen an otherwise concrete shape to `unknown`.
   if shape == "unknown" { return shapes }
   mut i = 0
   while i < shapes.len {
      if __load_item(shapes, i) == shape { return shapes }
      i += 1
   }
   return shapes.append(shape)
}

;; Keep shape/repr joining on the local dynamic path.  Calling the imported
;; typed `str.join` from this reflection module can cross the list ABI with
;; nested dynamic values while the type graph is still being inspected.
fn _join_parts(seq parts, str sep="") str {
   mut out = ""
   mut i = 0
   def int n = parts.len
   while i < n {
      if i > 0 { out = out + sep }
      ;; `parts` is a native list in JIT mode.  The legacy `__load_item`
      ;; inline path assumes the old managed +16 payload offset; the dynamic
      ;; accessor understands both layouts and preserves string handles.
      def part = __load_item_any(parts, i)
      out = out + part
      i += 1
   }
   return out
}

fn _type_shape_union_from_seq(seq xs, int depth) str {
   def int n = xs.len
   if n == 0 { return "empty" }
   mut out = ""
   mut i = 0
   while i < n {
      def shape = _type_shape(__load_item_any(xs, i), depth - 1)
      if shape != "unknown" {
         mut duplicate = false
         mut j = 0
         while j < i {
            if _type_shape(__load_item_any(xs, j), depth - 1) == shape {
               duplicate = true
               break
            }
            j += 1
         }
         if !duplicate {
            if out.len > 0 { out = out + "|" }
            out = out + shape
         }
      }
      i += 1
   }
   if out.len == 0 { return "unknown" }
   out
}

fn _type_shape_tuple(any xs, int depth) str {
   def int n = xs.len
   if n == 0 { return "tuple<>" }
   mut shapes = list()
   mut i = 0
   while i < n {
      shapes = shapes.append(_type_shape(__load_item_any(xs, i), depth - 1))
      i += 1
   }
   "tuple<" + _join_parts(shapes, ", ") + ">"
}

fn _type_shape_dict(dict d, int depth) str {
   def its = items(d)
   def int n = its.len
   if n == 0 { return "dict<empty, empty>" }
   mut key_shapes = list()
   mut val_shapes = list()
   mut i = 0
   while i < n {
      def pair = __load_item_any(its, i)
      key_shapes = _type_shape_union_add(key_shapes, _type_shape(__load_item_any(pair, 0), depth - 1))
      val_shapes = _type_shape_union_add(val_shapes, _type_shape(__load_item_any(pair, 1), depth - 1))
      i += 1
   }
   "dict<" + _join_parts(key_shapes, "|") + ", " +
   _join_parts(val_shapes, "|") + ">"
}

fn _type_shape(any x, int depth) str {
   if depth <= 0 {
      def out = type(x)
      return out
   }
   if _is_vecdict(x) { return _dict_get_raw(x, "__type", "dict") }
   if _is_list(x) { return "list<" + _type_shape_union_from_seq(x, depth) + ">" }
   if _is_tuple(x) { return _type_shape_tuple(x, depth) }
   if _is_dict(x) { return _type_shape_dict(x, depth) }
   if _is_set(x) { return "set<" + _type_shape_union_from_seq(items(x), depth) + ">" }
   def out = type(x)
   out
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
   if _is_list(spec) || _is_tuple(spec) {
      mut i = 0
      while i < spec.len {
         if is_shape(x, __load_item(spec, i), max_depth) { return true }
         i += 1
      }
      return false
   }
   if !_is_str(spec) { return false }
   type_shape(x, max_depth) == spec
}

fn _shape_spec_to_str(any spec) str {
   if _is_list(spec) || _is_tuple(spec) {
      return _join_parts(spec, "|")
   }
   to_str(spec)
}

fn require_shape(any x, any spec, str msg="shape check failed", int max_depth=6) any {
   "Returns `x` when it matches `spec`; otherwise panics with expected and actual shapes."
   if !is_shape(x, spec, max_depth) { __panic(msg + ": expected " + _shape_spec_to_str(spec) + ", got " + type_shape(x, max_depth)) }
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
   - **set**: adds element to set.
   - Other types: delegates to builtin `__add` (ints, floats, ptr math)."
   if _is_set(a) { return core_set._set_add(a, b) }
   if __is_int(a) {
      if __is_int(b) || _is_float(b) { return __add(a, b) }
   } elif _is_float(a) {
      if __is_int(b) || _is_float(b) { return __add(a, b) }
   }
   if _is_str(a) && _is_str(b) { return __add(a, b) }
   if _is_list_or_tuple(a) && _is_list_or_tuple(b) { return __add(a, b) }
   if _is_vecdict(a) {
      if _is_vecdict(b) { return _vec_zip2(a, b, 0) }
   }
   return __add(a, b)
}

fn sub(any a, any b) any {
   "Generic subtraction with list support.
   - **set**: removes element from set.
   - **list/tuple - list/tuple**: element-wise difference(min length).
   - Other types: delegates to builtin `__sub`."
   if _is_set(a) { return core_set._set_remove(a, b) }
   if __is_int(a) {
      if __is_int(b) || _is_float(b) { return __sub(a, b) }
   } elif _is_float(a) {
      if __is_int(b) || _is_float(b) { return __sub(a, b) }
   }
   if _is_list_or_tuple(a) && _is_list_or_tuple(b) { return _list_zip2(a, b, 1) }
   if _is_vecdict(a) {
      if _is_vecdict(b) { return _vec_zip2(a, b, 1) }
   }
   return __sub(a, b)
}

@returns_owned
fn _repeat_seq_like(any xs, int n) any {
   def orig_len = xs.len
   mut out = list(orig_len * n)
   mut rep = 0
   while rep < n {
      mut j = 0
      while j < orig_len {
         _store_item_raw(out, rep * orig_len + j, __load_item(xs, j))
         j += 1
      }
      rep += 1
   }
   _set_seq_count(out, orig_len * n)
   if _is_tuple(xs) { list_as_tuple_raw(out) }
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
   if __is_int(a) {
      if __is_int(b) || _is_float(b) { return __mul(a, b) }
   } elif _is_float(a) {
      if __is_int(b) || _is_float(b) { return __mul(a, b) }
   }
   if __is_int(b) && b > 0 {
      if _is_str(a) {
         return repeat(a, b)
      }
      if _is_list_or_tuple(a) {
         return _repeat_seq_like(a, b)
      }
   }
   if __is_int(a) && a > 0 {
      if _is_list_or_tuple(b) {
         return _repeat_seq_like(b, a)
      }
   }
   if _is_list_or_tuple(a) {
      if _is_mat4(a) {
         if _is_list_or_tuple(b) && b.len == 16 { return _mat4_mul(a, b) }
         if _is_list_or_tuple(b) && b.len == 4 { return _mat4_mul_vec4(a, b) }
      }
      if _is_float(b) { return _list_scale(a, b, 0) }
      if _is_list_or_tuple(b) { return _list_zip2(a, b, 2) }
   }
   if _is_vecdict(a) {
      if _is_vecdict(b) { return _vec_dot(a, b) }
      if __is_int(b) || _is_float(b) { return _vec_scale(a, b, 0) }
   }
   if _is_vecdict(b) { if __is_int(a) || _is_float(a) { return _vec_scale(b, a, 0) } }
   return __mul(a, b)
}

fn div(any a, any b) any {
   "Generic division.
   - **list/tuple / list/tuple**: element-wise division(min length).
   - **list/tuple / scalar**: divide each element by scalar.
   - Other types: delegates to builtin `__div`."
   if _is_list_or_tuple(a) && _is_list_or_tuple(b) { return _list_zip2(a, b, 3) }
   if _is_list_or_tuple(a) && (__is_int(b) || _is_float(b)) { return _list_scale(a, b, 1) }
   if _is_vecdict(a) {
      if _is_vecdict(b) { return _vec_zip2(a, b, 3) }
      if __is_int(b) || _is_float(b) { return _vec_scale(a, b, 1) }
   }
   return __div(a, b)
}

@inline
fn _is_list_or_tuple(any x) bool {
   def tag = __tagof(x)
   if !__is_int(tag) { return false }
   return (tag == _TAG_LIST) || (tag == _TAG_TUPLE)
}

@returns_owned
fn _list_like(int n) list { list(n) }

@returns_owned
fn _list_zip2(any a, any b, int op) any {
   def int na, nb = a.len, b.len
   def int n = (na < nb) ? na : nb
   def want_tuple = _is_tuple(a) && _is_tuple(b)
   mut out = _list_like(n)
   mut i = 0
   while i < n {
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
   if want_tuple { list_as_tuple_raw(out) }
   return out
}

@returns_owned
fn _list_scale(any a, any s, int op) any {
   def int n = a.len
   def want_tuple = _is_tuple(a)
   mut out, i = _list_like(n), 0
   while i < n {
      def x, z = __load_item(a, i), case op {
         0 -> __mul(x, s)
         _ -> __div(x, s)
      }
      _store_item_raw(out, i, z)
      i += 1
   }
   __store64_idx(out, 0, n)
   if want_tuple { list_as_tuple_raw(out) }
   return out
}

fn _is_mat4(any x) bool { return _is_list_or_tuple(x) && x.len == 16 }

@returns_owned
fn _mat4_mul(any a, any b) list {
   mut out = list(16)
   mut r = 0
   while r < 4 {
      mut c = 0
      while c < 4 {
         mut s, k = 0, 0
         while k < 4 {
            s = s + __load_item(a, r * 4 + k) * __load_item(b, k * 4 + c)
            k += 1
         }
         out = out.append(s)
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
   while r < 4 {
      mut s, c = 0, 0
      while c < 4 {
         s = s + __load_item(m, r * 4 + c) * __load_item(v, c)
         c += 1
      }
      out = out.append(s)
      r += 1
   }
   return out
}

fn list_eq(any a, any b) bool {
   "Performs deep structural equality comparison for two lists."
   def na, nb = __load64_idx(a, 0), __load64_idx(b, 0)
   if !(na == nb) { return false }
   mut i = 0
   while i < na {
      def va, vb = __load_item(a, i), __load_item(b, i)
      if !eq(va, vb) { return false }
      i += 1
   }
   return true
}

fn _seq_eq(any a, any b) bool {
   def na, nb = a.len, b.len
   if !(na == nb) { return false }
   mut i = 0
   while i < na {
      if !eq(__load_item(a, i), __load_item(b, i)) { return false }
      i += 1
   }
   return true
}

fn dict_eq(any a, any b) bool {
   "Performs deep structural equality comparison for two dictionaries."
   if !(a.len == b.len) { return false }
   def its = items(a)
   mut i = 0
   def int n = its.len
   while i < n {
      def pair = __load_item(its, i)
      def k = __load_item(pair, 0)
      if !_dict_has_raw(b, k) { return false }
      if !eq(_dict_get_raw(b, k, 0), __load_item(pair, 1)) { return false }
      i += 1
   }
   return true
}

fn set_eq(any a, any b) bool {
   "Performs deep structural equality comparison for two sets."
   if !(a.len == b.len) { return false }
   def its = items(a)
   mut i = 0
   def int n = its.len
   while i < n {
      if !core_set.contains(b, __load_item(its, i)) { return false }
      i += 1
   }
   return true
}

fn _eq_impl(any a, any b) bool {
   "Structural equality operator. Compares values by content(strings/collections) or identity(primitives)."
   def same = (a == b)
   if same { return true }
   def ta, tb = __tagof(a), __tagof(b)
   if !__is_int(ta) { return false }
   if !__is_int(tb) { return false }
   if _is_str_tag(ta) {
      if !_is_str_tag(tb) { return false }
      return _str_eq(a, b)
   }
   def a_seq, b_seq = _is_seq_tag(ta), _is_seq_tag(tb)
   def a_list_tuple, b_list_tuple = _is_list_tuple_tag(ta), _is_list_tuple_tag(tb)
   if a_list_tuple && !b_list_tuple { return false }
   if !a_list_tuple && b_list_tuple { return false }
   if a_seq { if b_seq { return _seq_eq(a, b) } }
   ; Keep tag dispatch on the intrinsic resolver and raw integer comparison;
   ; resolving a tag through the overloaded comparator re-enters structural
   ; equality in the REPL's incremental context.
   if ta == _TAG_DICT {
      if tb == _TAG_DICT {
         def av, bv = _is_vecdict(a), _is_vecdict(b)
         if av {
            if bv { return _vec_eq(a, b) }
            return false
         }
         if bv { return false }
      }
   }
   if (ta < 100) || (ta > 255) || (tb < 100) || (tb > 255) { return false }
   if ta == tb {
      if (ta == _TAG_LIST) || (ta == _TAG_TUPLE) { return list_eq(a, b) }
      if ta == _TAG_DICT { return dict_eq(a, b) }
      if ta == _TAG_SET { return set_eq(a, b) }
      if ta == _TAG_RANGE { return _seq_eq(a, b) }
      if ta == _TAG_FLOAT { return __flt_eq(a, b) }
      if ta == _TAG_BIGINT { return (__bigint_cmp(a, b) == 0) }
      if ta == _TAG_BIGFLOAT { return (__bigfloat_cmp(a, b) == 0) }
      return false
   } else {
      return false
   }
}

@jit
fn eq(any a, any b) bool {
   return _eq_impl(a, b)
}

fn _repr_seq(any xs, str open, str close, int depth=0) str {
   def int n = xs.len
   if n == 0 { return open + close }
   ;; Concatenate directly: intermediate part lists corrupted
   ;; mixed-representation contents (float elements) on the current
   ;; lowering.
   mut out = open
   mut i = 0
   while i < n {
      if i > 0 { out = out + ", " }
      out = out + _repr_depth(__load_item_any(xs, i), depth + 1)
      i += 1
   }
   out + close
}

fn _repr_items(any its, bool pairs, str open="{", str close="}", int depth=0) str {
   def int n = its.len
   if n == 0 { return open + close }
   mut out = open
   mut i = 0
   while i < n {
      if i > 0 { out = out + ", " }
      if pairs {
         def p = __load_item(its, i)
         out = out + _repr_depth(__load_item(p, 0), depth + 1) + ": " +
         _repr_depth(__load_item(p, 1), depth + 1)
      } else {
         out = out + _repr_depth(__load_item(its, i), depth + 1)
      }
      i += 1
   }
   out + close
}

fn _repr_depth(any x, int depth) str {
   if __is_int(x) { return __to_str(x) }
   if x == true { return "true" }
   if x == false { return "false" }
   ;; Unboxed and boxed floats must resolve before the generic non-pointer
   ;; nil guard below, which would otherwise swallow them.
   if _is_float(x) { return to_str(x) }
   if !__is_ptr(x) && !_is_str(x) { return "nil" }
   if _is_str(x) { return f"\"{x}\"" }
   ;; Keep native sequence formatting on the sequence ABI before any generic
   ;; object/string fallback, matching _to_str_depth above.
   if _is_list(x) {
      if depth >= 4 { return "[...]" }
      return _repr_seq(x, "[", "]", depth)
   }
   if _is_tuple(x) {
      if depth >= 4 { return "(...)" }
      return _repr_seq(x, "(", ")", depth)
   }
   if !__is_ny_obj(x) { return to_str(x) }
   def kind = __tagof(x)
   if _is_bigint(x) { return __bigint_to_str(x) }
   if _is_bigfloat(x) { return __bigfloat_to_str(x) }
   if _is_vecdict(x) { return _vec_to_str(x) }
   if _is_range(x) {
      def start = __load64_idx(x, 0)
      def stop = __load64_idx(x, 8)
      def step = __load64_idx(x, 16)
      return f"range({start}, {stop}, {step})"
   }
   if _is_dict(x) {
      if depth >= 4 { return "{...}" }
      return _repr_items(items(x), true, "{", "}", depth)
   }
   if _is_set(x) {
      if depth >= 4 { return "{...}" }
      return _repr_items(items(x), false, "{", "}", depth)
   }
   if kind == _TAG_FLOAT { return to_str(x) }
   if kind == _TAG_COMPLEX { return __to_str(x) }
   if _is_bytes(x) { return f"<bytes {_raw_len(x)}>" }
   if _is_bigint(x) { return __bigint_to_str(x) }
   if _is_bigfloat(x) { return __bigfloat_to_str(x) }
   "<ptr " + __ptr_key(x) + " tag=" + __ptr_key(__tagof(x)) + ">"
}

fn repr(any x) str {
   "Returns a programmer-friendly string representation of value `x`.
   - Strings are quoted.
   - Collections are shown with their structural contents and recursion guards.
   - Primitives are shown as their literal values."
   _repr_depth(x, 0)
}

fn _hash_mix(int h, any v) int {
   ((h ^ hash(v)) * 16777619) & 2147483647
}

fn hash(any x) int {
   "Returns a stable structural hash for primitive and collection values."
   if is_none(x) { return 0x4e494c }
   if __is_int(x) { return x }
   if _is_str(x) { return __str_hash(x) }
   if _is_float(x) { return __flt_hash(x) }
   if _is_bigint(x) { return __str_hash(__bigint_to_str(x)) }
   if _is_bigfloat(x) { return __str_hash(__bigfloat_to_str(x)) }
   if _is_range(x) {
      mut h = 2166136261
      h = _hash_mix(h, __load64_idx(x, 0))
      h = _hash_mix(h, __load64_idx(x, 8))
      h = _hash_mix(h, __load64_idx(x, 16))
      return h
   }
   if _is_list(x) || _is_tuple(x) {
      mut h, i = 2166136261, 0
      while i < x.len {
         h = _hash_mix(h, __load_item(x, i))
         i += 1
      }
      if _is_tuple(x) { h = _hash_mix(h, 0x5455504c) }
      return h
   }
   if _is_dict(x) {
      mut h = 2166136261
      def its = items(x)
      mut i = 0
      while i < its.len {
         def p = __load_item(its, i)
         h = _hash_mix(h, __load_item(p, 0))
         h = _hash_mix(h, __load_item(p, 1))
         i += 1
      }
      return h
   }
   if _is_set(x) {
      mut h = 2166136261
      def its = items(x)
      mut i = 0
      while i < its.len {
         h = h ^ hash(__load_item(its, i))
         i += 1
      }
      return h & 2147483647
   }
   return 0
}

@jit
fn globals() any {
   "Returns the runtime global registry installed with set_globals; lexical and imported bindings are not added automatically."
   return __globals()
}

@returns_owned
fn items(any x) list {
   "Generic item iterator. Returns a list of `[index/key, value]` pairs."
   if _is_vecdict(x) {
      def n = _vec_dim(x)
      mut out = list(n)
      mut i = 0
      while i < n {
         _store_item_raw(out, i, [i, _vec_at(x, i)])
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   if _is_dict(x) { return _dict_items_raw(x) }
   if _is_set(x) {
      def cap = __load64_idx(x, 8)
      mut out = list(cap)
      mut i = 0
      while i < cap {
         def off = 16 + i * 24
         if __load64_idx(x, off + 16) == 1 { out = out.append(__load64_idx(x, off)) }
         i += 1
      }
      return out
   }
   if _is_range(x) {
      def start = __load64_idx(x, 0)
      def stop = __load64_idx(x, 8)
      def step = __load64_idx(x, 16)
      mut n = 0
      if step > 0 && start < stop { n = (stop - start + step - 1) / step }
      elif step < 0 && start > stop { n = (start - stop - step - 1) / (0 - step) }
      mut out = list(n)
      mut cur = start
      mut i = 0
      while i < n {
         _store_item_raw(out, i, [i, cur])
         cur = cur + step
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   if _is_seq(x) || _is_str(x) {
      def int n = x.len
      mut out = list(n)
      mut i = 0
      while i < n {
         _store_item_raw(out, i, [i, __load_item(x, i)])
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
   if _is_vecdict(x) {
      def n = _vec_dim(x)
      mut out = list(n)
      mut i = 0
      while i < n {
         _store_item_raw(out, i, i)
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   if _is_dict(x) { return _dict_keys_raw(x) }
   if _is_set(x) { return items(x) }
   if _is_range(x) {
      def start = __load64_idx(x, 0)
      def stop = __load64_idx(x, 8)
      def step = __load64_idx(x, 16)
      mut n = 0
      if step > 0 && start < stop { n = (stop - start + step - 1) / step }
      elif step < 0 && start > stop { n = (start - stop - step - 1) / (0 - step) }
      mut out = list(n)
      mut i = 0
      while i < n {
         _store_item_raw(out, i, i)
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   if _is_seq(x) || _is_str(x) {
      def int n = x.len
      mut out = list(n)
      mut i = 0
      while i < n {
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
   if _is_vecdict(x) {
      def n = _vec_dim(x)
      mut out = list(n)
      mut i = 0
      while i < n {
         _store_item_raw(out, i, _vec_at(x, i))
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   if _is_dict(x) { return _dict_values_raw(x) }
   if _is_set(x) { return items(x) }
   if _is_range(x) {
      def start = __load64_idx(x, 0)
      def stop = __load64_idx(x, 8)
      def step = __load64_idx(x, 16)
      mut n = 0
      if step > 0 && start < stop { n = (stop - start + step - 1) / step }
      elif step < 0 && start > stop { n = (start - stop - step - 1) / (0 - step) }
      mut cur = start
      mut out = list(n)
      mut i = 0
      while i < n {
         _store_item_raw(out, i, cur)
         cur = cur + step
         i += 1
      }
      _set_seq_count(out, n)
      return out
   }
   if _is_list(x) || _is_tuple(x) || _is_str(x) {
      def int n = x.len
      mut out = list(n)
      mut i = 0
      while i < n {
         _store_item_raw(out, i, __load_item(x, i))
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
   if _is_list(obj) || _is_tuple(obj) {
      ;; Keep list indices in the language integer ABI.  `key` and
      ;; `__list_len` are tagged values here; untagging `key` into an `int`
      ;; local made odd raw indices (notably 1) look tagged a second time at
      ;; `__load_item_any`, so interactive/JIT reads repeatedly selected slot
      ;; zero.
      mut int k = 0
      if __is_int(key) { k = __untag(key) }
      elif _is_bigint(key) { k = __bigint_to_int(key) }
      else { return _index_read_key_error(key) }
      def n = __list_len(obj)
      if k < 0 { k = k + n }
      if k < 0 || k >= n { return _index_read_oob_error(k, n) }
      _index_read_probe(tag, k, 0)
      return __load_item_any(obj, k)
   }
   if _is_dict(obj) {
      if _is_vecdict(obj) {
         mut int k = 0
         if __is_int(key) { k = __untag(key) }
         elif _is_bigint(key) { k = __bigint_to_int(key) }
         else { return _index_read_key_error(key) }
         def n = _vec_dim(obj)
         if k < 0 { k = k + n }
         if k < 0 || k >= n { return _index_read_oob_error(k, n) }
         _index_read_probe(tag, k, 0)
         return _vec_at(obj, k, 0)
      }
      _index_read_probe(tag, key, 0)
      return _dict_get_raw(obj, key, 0)
   }
   if _is_str(obj) || __is_str_obj(obj) {
      mut int k = 0
      if __is_int(key) { k = __untag(key) }
      elif _is_bigint(key) { k = __bigint_to_int(key) }
      else { return _index_read_key_error(key) }
      use core.str
      def total = utf8_len(obj)
      if k < 0 { k = k + total }
      if k < 0 || k >= total { return _index_read_oob_error(k, total) }
      _index_read_probe(tag, k, 0)
      return chr(ord_at(obj, k))
   }
   if _is_bytes(obj) {
      mut int k = 0
      if __is_int(key) { k = __untag(key) }
      elif _is_bigint(key) { k = __bigint_to_int(key) }
      else { return _index_read_key_error(key) }
      def int n = obj.len
      if k < 0 { k = k + n }
      if k < 0 || k >= n { return _index_read_oob_error(k, n) }
      _index_read_probe(tag, k, 0)
      return load8(obj, k)
   }
   if _is_range(obj) {
      mut int k = 0
      if __is_int(key) { k = __untag(key) }
      elif _is_bigint(key) { k = __bigint_to_int(key) }
      else { return _index_read_key_error(key) }
      def int n = obj.len
      if k < 0 { k = k + n }
      if k < 0 || k >= n { return _index_read_oob_error(k, n) }
      _index_read_probe(tag, k, 0)
      def start = __load64_idx(obj, 0)
      def step = __load64_idx(obj, 16)
      return start + k * step
   }
   if obj == __globals() { return 0 }
   _index_read_type_error(obj)
}

fn get(any obj, any key, any default=0) any {
   "Generic element retriever. Handles indexing for strings, lists, dicts, tuples, ranges, and vectors.
   - `obj`: Collection(str, list, dict, tuple, range, vector)
   - `key`: Index or Key
   - `default`: Value to return if key/index not found(default 0).
   "
   ; Keep integer-key access on the runtime's raw-index bridge. It resolves
   ; compact native handles, forwarded tbufs, and managed sequences before
   ; checking their layouts, which source-level header probes cannot do for
   ; every backend representation.
   if __is_int(key) {
      mut int k = __untag(key)
      return __value_get_index_raw(obj, k, default)
   }
   if _is_bigint(key) {
      mut int k = __bigint_to_int(key)
      return __value_get_index_raw(obj, k, default)
   }
   ; Native tbuf lists use raw NYIR indices.  The generic `__is_int` test
   ; interprets odd raw indices as tagged values (1 becomes 0), so dispatch
   ; before the legacy list branch.
   if (_is_list(obj) || _is_tuple(obj)) &&
   __load64_idx(obj, -32) == 0x4e59544255464d47 {
      return default
   }
   if _is_list(obj) || _is_tuple(obj) {
      mut int k = 0
      if __is_int(key) { k = __untag(key) }
      elif _is_bigint(key) { k = __bigint_to_int(key) }
      else { return default }
      def n = __load64_idx(obj, 0)
      if k < 0 { k = k + n }
      if k < 0 || k >= n { return default }
      else { return __load_item(obj, k) }
   }
   if _is_dict(obj) {
      if _is_vecdict(obj) {
         if __is_int(key) { return _vec_at(obj, __untag(key), default) }
         if _is_bigint(key) { return _vec_at(obj, __bigint_to_int(key), default) }
      }
      return _dict_get_raw(obj, key, default)
   }
   if _is_str(obj) || __is_str_obj(obj) {
      mut int k = 0
      if __is_int(key) { k = __untag(key) }
      elif _is_bigint(key) { k = __bigint_to_int(key) }
      else { return default }
      use core.str
      def total = utf8_len(obj)
      if k < 0 { k = k + total }
      if k < 0 || k >= total { return default }
      else {
         return chr(ord_at(obj, k))
      }
   }
   if _is_bytes(obj) {
      mut int k = 0
      if __is_int(key) { k = __untag(key) }
      elif _is_bigint(key) { k = __bigint_to_int(key) }
      else { return default }
      def int n = obj.len
      if k < 0 { k = k + n }
      if k < 0 || k >= n { return default }
      else { return load8(obj, k) }
   }
   if _is_range(obj) {
      mut int k = 0
      if __is_int(key) { k = __untag(key) }
      elif _is_bigint(key) { k = __bigint_to_int(key) }
      else { return default }
      def int n = obj.len
      if k < 0 { k = k + n }
      if k < 0 || k >= n { return default }
      else {
         def start = __load64_idx(obj, 0)
         def step = __load64_idx(obj, 16)
         return start + k * step
      }
   }
   if obj == __globals() { return default }
   if !obj || _is_raw_ptr_like(obj) { return default }
   _type_error("get", "a string, bytes, list, tuple, dict, range, or vector", obj)
}

fn _set_impl(any obj, any key, any val) any {
   if !obj || _is_raw_ptr_like(obj) { return 0 }
   if _is_vecdict(obj) {
      if __is_int(key) { return _vec_set(obj, __untag(key), val) }
      if _is_bigint(key) { return _vec_set(obj, __bigint_to_int(key), val) }
   }
   if _is_dict(obj) { return _dict_put_raw(obj, key, val) }
   elif _is_list(obj) {
      mut int k = 0
      if __is_int(key) { k = int(key) }
      elif _is_bigint(key) { k = __bigint_to_int(key) }
      else { _type_error("set", "an integer index", key) }
      def int n = obj.len
      if k < 0 { k = k + n }
      if k < 0 || k >= n {
         def cap = __load64_idx(obj, 8)
         if k == n && k < cap {
            __store_item_fast(obj, k, val)
            store64(obj, n + 1, 0)
            return obj
         }
         _index_error("set", k, n)
      }
      else {
         __store_item_fast(obj, k, val)
         return obj
      }
   }
   elif _is_bytes(obj) {
      mut int k = 0
      if __is_int(key) { k = __untag(key) }
      elif _is_bigint(key) { k = __bigint_to_int(key) }
      else { _type_error("set", "an integer index", key) }
      def int n = obj.len
      if k < 0 { k = k + n }
      if k < 0 || k >= n { _index_error("set", k, n) }
      store8(obj, val, k)
      return obj
   }
   else { _type_error("set", "a bytes, list, dict, or vector", obj) }
}

fn set_idx(any obj, any key, any val) any {
   "Generic element setter. Supported for dicts, lists, bytes, and vectors. Returns the object or 0 on failure."
   return _set_impl(obj, key, val)
}

fn set(any obj, any key, any val) any {
   "Generic setter for dicts, lists, bytes, and vectors. Returns the mutated object for method chaining."
   return _set_impl(obj, key, val)
}

@returns_owned
fn slice(any obj, int start, any stop=nil, int step=1) any {
   "Generic **slice** operation for strings, lists, and tuples."
   if is_nil(stop) {
      stop = obj.len
   }
   if _is_str(obj) || __is_str_obj(obj) || is_str(obj) {
      ;; `start` and `step` are already statically raw integers. Re-running
      ;; `int()` on them can route through the tagged-any decoder and halve
      ;; odd bounds (for example slice("abcd", 1, 3) started at zero).
      return utf8_slice(obj, start, int(stop), step)
   }
   elif _is_list(obj) || _is_tuple(obj) {
      def want_tuple = _is_tuple(obj)
      def int n = obj.len
      if start < 0 { start = n + start }
      if stop < 0 { stop = n + stop }
      if step > 0 {
         if start < 0 { start = 0 }
         if stop > n { stop = n }
         if start >= stop {
            mut empty = list(0)
            if want_tuple { core_prim.list_as_tuple_raw(empty) }
            return empty
         }
      } else {
         if start >= n { start = n - 1 }
         if stop < core_prim.sub(0, 1) { stop = core_prim.sub(0, 1) }
         if start <= stop {
            mut empty = list(0)
            if want_tuple { core_prim.list_as_tuple_raw(empty) }
            return empty
         }
      }
      mut out = list(n)
      mut i = start
      if step > 0 {
         while i < stop {
            out = out.append(__load_item(obj, i))
            i = i + step
         }
      } else {
         while i > stop {
            out = out.append(__load_item(obj, i))
            i = i + step
         }
      }
      if want_tuple { core_prim.list_as_tuple_raw(out) }
      return out
   }
   else { _type_error("slice", "a string, list, or tuple", obj) }
}

fn append(any lst, any v) any {
   "Appends value `v` to the end of list `lst`. Returns the(possibly reallocated) list ptr."
   if !_is_list(lst) { _type_error("append", "a list", lst) }
   else { return __append(lst, v) }
}

fn pop(any lst) any {
   "Removes and returns the last element from list `lst`. Returns `0` if empty."
   if !_is_list(lst) { _type_error("pop", "a list", lst) }
   ; Native typed buffers keep their count in the out-of-line header.  The
   ; legacy load/store sequence below reads and writes slot zero instead.
   elif __load64_idx(lst, -32) == 0x4e59544255464d47 {
      return __tbuf_pop_any(lst)
   }
   else {
      def n = __load64_idx(lst, 0)
      if n == 0 { return 0 }
      else {
         def v = __load_item(lst, n - 1)
         __store64_idx(lst, 0, n - 1)
         return v
      }
   }
}

@returns_owned
fn _extend_str_owned(str lst, any other) str {
   def int n = other.len
   mut b = Builder(lst.len + n * 4 + 8)
   b = builder_append(b, lst)
   mut i = 0
   while i < n {
      b = builder_append(b, __load_item(other, i))
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   return out
}

@returns_owned
fn _extend_list_realloc(any lst, any other, int ln, int on) list {
   mut out = list(ln + on)
   mut i = 0
   while i < ln {
      _store_item_raw(out, i, __load_item(lst, i))
      i += 1
   }
   i = 0
   while i < on {
      _store_item_raw(out, ln + i, __load_item(other, i))
      i += 1
   }
   _set_seq_count(out, ln + on)
   out
}

fn extend(any lst, any other) any {
   "Appends all elements from collection `other` to `lst`.
   - list target: appends items and returns a list
   - string target: concatenates items and returns a string"
   if _is_str(lst) { return _extend_str_owned(lst, other) }
   if !_is_list(lst) { return lst }
   ; Native buffers use the runtime's alias-aware growth path.
   elif __load64_idx(lst, -32) == 0x4e59544255464d47 {
      return __tbuf_extend(lst, other)
   }
   def ln, on = lst.len, other.len
   def cap = __load64_idx(lst, 8)
   if ln + on <= cap {
      mut i = 0
      while i < on {
         _store_item_raw(lst, ln + i, __load_item(other, i))
         i += 1
      }
      _set_seq_count(lst, ln + on)
      return lst
   }
   _extend_list_realloc(lst, other, ln, on)
}

fn _str_seq_depth(any xs, str open, str close, int depth) str {
   def int n = xs.len
   if n == 0 { return open + close }
   mut out = open
   mut i = 0
   while i < n {
      if i > 0 { out = out + ", " }
      out = out + _to_str_depth(__load_item_any(xs, i), depth + 1)
      i += 1
   }
   out + close
}

fn _str_items_depth(any its, bool pairs, int depth, str open="{", str close="}") str {
   def int n = its.len
   if n == 0 { return open + close }
   mut out = open
   mut i = 0
   while i < n {
      if i > 0 { out = out + ", " }
      if pairs {
         def p = __load_item(its, i)
         out = out + _to_str_depth(__load_item(p, 0), depth + 1) + ": " +
         _to_str_depth(__load_item(p, 1), depth + 1)
      } else {
         out = out + _to_str_depth(__load_item(its, i), depth + 1)
      }
      i += 1
   }
   out + close
}

fn _to_str_depth(any v, int depth) str {
   if v == true { return "true" }
   if v == false { return "false" }
   if __is_int(v) { return __to_str(v) }
   if !v { return "nil" }
   if _is_str(v) { return v }
   ;; Native lists/tuples are valid Nytrix objects but are represented by the
   ;; tbuf ABI rather than the legacy heap-object header.  Dispatch structural
   ;; sequences before the generic object fallback so REPL/JIT code cannot
   ;; accidentally stringify a native sequence as an opaque pointer.
   if _is_list(v) {
      if depth >= 4 { return "[...]" }
      return _str_seq_depth(v, "[", "]", depth)
   }
   if _is_tuple(v) {
      if depth >= 4 { return "(...)" }
      return _str_seq_depth(v, "(", ")", depth)
   }
   def kind = __tagof(v)
   if !__is_ny_obj(v) { return __to_str(v) }
   if _is_bigint(v) { return __bigint_to_str(v) }
   if _is_bigfloat(v) { return __bigfloat_to_str(v) }
   if _is_vecdict(v) { return _vec_to_str(v) }
   if _is_range(v) {
      def start = __load64_idx(v, 0)
      def stop = __load64_idx(v, 8)
      def step = __load64_idx(v, 16)
      return f"range({start}, {stop}, {step})"
   }
   if _is_dict(v) {
      if depth >= 4 { return "{...}" }
      return _str_items_depth(items(v), true, depth)
   }
   if _is_set(v) {
      if depth >= 4 { return "{...}" }
      return _str_items_depth(items(v), false, depth)
   }
   if _is_bytes(v) { return f"<bytes {_raw_len(v)}>" }
   if kind == _TAG_FLOAT { return __to_str(v) }
   if kind == _TAG_COMPLEX { return __to_str(v) }
   if _is_bigint(v) { return __bigint_to_str(v) }
   if _is_bigfloat(v) { return __bigfloat_to_str(v) }
   "<ptr " + __ptr_key(v) + " tag=" + __ptr_key(__tagof(v)) + ">"
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
      if !cond { __panic(msg) }
      0
   }
   _reflect_check(type(42) == "int" && type("hello") == "str" && type([1, 2, 3]) == "list" && type(dict(8)) == "dict", "reflect types")
   _reflect_check(len([1, 2, 3]) == 3 && len("hello") == 5, "reflect len")
   _reflect_check(contains([1, 2, 3], 2) && contains("hello world", "world"), "reflect contains")
   _reflect_check(eq([1, 2, 3], [1, 2, 3]) && !eq([1, 2, 3], [1, 2, 4]), "reflect list equality")
   _reflect_check(eq((1, 2), (1, 2)) && eq((1, 2), [1, 2]), "reflect tuple/list sequence equality")
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
   def opaque_err = exception(ERR_RUNTIME, "opaque")
   _reflect_check(to_str(opaque_err).len > 0 && repr(opaque_err).len > 0, "reflect opaque object formatting")
   _reflect_check(hash("hello") == hash("hello") && hash("hello") != hash("world") && hash([1, 2]) == hash([1, 2]), "reflect hash")
   def deep_list = [[[[[1]]]]]
   _reflect_check(repr(deep_list).contains("[...]") && to_str(deep_list).contains("[...]"), "reflect deep list rendering")
   def deep_dict = {"a": {"b": {"c": {"d": {"e": 1}}}}}
   _reflect_check(repr(deep_dict).contains("{...}") && to_str(deep_dict).contains("{...}"), "reflect deep dict rendering")
   def writable = [1, 2, 3]
   writable[1] = 9
   _reflect_check(writable[1] == 9, "reflect set_idx raw store")
   _reflect_check(slice((1, 2, 3), 1, 3) == (2, 3), "reflect tuple slice")
   def v2, v3, v4 = Vector2([3, 4]), Vector3([1, 2, 3]), Vector4([5, 6, 7, 8])
   _reflect_check(len(v2) == 2 && len(v3) == 3 && len(v4) == 4, "reflect vector dimensions")
   _reflect_check(get(v2, "__type", "") == "vec2" && get(v3, "__type", "") == "vec3" && get(v4, "__type", "") == "vec4", "reflect vector type markers")
   _reflect_check(contains((1, 2, 3), 3), "reflect tuple contains")
   def pair_product = mul([2, 3], [4, 5])
   def pair_quotient = div([8, 9], [2, 3])
   _reflect_check(pair_product[0] == 8 && pair_product[1] == 15, "reflect list pair multiplication")
   _reflect_check(pair_quotient[0] == 4 && pair_quotient[1] == 3, "reflect list pair division")
   def repeat_result = mul((6, 7), 2)
   _reflect_check(repeat_result.len == 4 && repeat_result[2] == 6 && repeat_result[3] == 7, "reflect tuple repeat")
   print("✓ core.reflect self-test passed")
}
