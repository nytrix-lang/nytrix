;; Runtime type surface: typed bindings, language type groups, impl self,
;; operators, and layouts.
use std.core
use std.core.set_mod as set_mod
use std.core.iter as it
use std.core.syntax.type as ty
use std.math.vector as vec
use std.math.bin
use std.math.nt
use std.os.ui.window.platform.contract as contract
use std.core.str
use std.math.crypto.encoding.bytes
use std.core.io
use std.core.tbuf

fn test_add(int a, int b) int {
   return a + b
}

fn get_name() str {
   return "John"
}

fn process(str data) str {
   return data
}

fn maybe_int(bool flag) ?int {
   if flag { return 7 }
   return nil
}

fn read_opt(?int v) ?int {
   return v
}

fn maybe_list(bool flag) ?list {
   if flag { return [4, 5] }
   return nil
}

fn nullable_list_after_guard() int {
   def ?list raw = maybe_list(true)
   if raw == nil { return 0 }
   def list xs = raw
   xs.len
}

fn nullable_list_after_reversed_guard() int {
   def ?list raw = [7, 8, 9]
   if nil == raw { return 0 }
   def list xs = raw
   xs.len
}

fn nullable_list_after_else_guard() int {
   def ?list raw = [1]
   if raw != nil {
   }else {
      return 0
   }
   def list xs = raw
   xs.len
}

fn need_int(int v) int {
   return v
}

;; Inference audit: dynamic float result.
fn dynamic_float_size() {
   15.5
}

fn is_nonzero(int v) bool {
   return v != 0
}

fn echo_int_ptr(*int p) *int {
   return p
}

fn read_int_ptr(*int p) int {
   if p == nil { return 0 }
   return load64(p)
}

fn write_int_ptr(*int p, int v) *int {
   if p != nil { store64(p, v) }
   return p
}

fn echo_handle(handle h) handle {
   return h
}

fn read_opt_handle(?handle h) ?handle {
   return h
}

fn typed_f64_add(f64 a, f64 b) f64 {
   return a + b
}

fn typed_f32_f64_mix(f32 a, f64 b) f64 {
   return a + b
}

fn typed_f64_tail(f64 a, f64 b) f64 {
   a * b + 1.0
}

fn typed_list_size(list xs) int {
   xs.len
}

fn typed_dict_size(dict d) int {
   d.len
}

fn typed_tuple_size(tuple t) int {
   t.len
}

fn typed_set_has_x(set s) bool {
   contains(s, "x")
}

fn typed_bytes_size(bytes b) int {
   b.len
}

fn typed_list_index_sum(list xs) int {
   mut i = 0
   mut acc = 0
   while i < xs.len {
      acc += xs[i]
      i += 1
   }
   acc
}

fn typed_nested_index_sum(list rows) int {
   mut i = 0
   mut acc = 0
   while i < rows.len {
      def row = rows[i]
      acc += row[0] + row[-1]
      i += 1
   }
   acc
}

fn strict_bare_list_sum(list xs) int {
   mut i = 0
   mut acc = 0
   while i < xs.len {
      acc += xs[i]
      i += 1
   }
   acc
}

fn strict_bare_nested_list_sum(list rows) int {
   mut i = 0
   mut acc = 0
   while i < rows.len {
      def row = rows[i]
      acc += row[0] + row[-1]
      i += 1
   }
   acc
}

fn strict_bare_dict_sum(dict d) int {
   d["x"] + d["y"]
}

fn strict_bare_float_list_sum(list xs) f64 {
   mut f64 acc = 0.0
   mut i = 0
   while i < xs.len {
      acc += xs[i]
      i += 1
   }
   acc
}

fn echo_any(any x) any {
   x
}

fn any_type_name(any x) str {
   type(x)
}

fn nullable_list_index_sum() int {
   def ?list raw = [2, 4, 6]
   if raw == nil { return 0 }
   def list xs = raw
   mut i = 0
   mut acc = 0
   while i < xs.len {
      acc += xs[i]
      i += 1
   }
   acc
}

fn test_primitives() {
   def i8 a = 10
   def i16 b = 20
   def i32 c = 30
   def i64 d = 40
   def u8 e = 50
   def u16 f = 60
   def u32 g = 70
   def u64 h = 80
   def char i = 'A'
   def bool j = true
   assert(a == 10, "i8 failed")
   assert(b == 20, "i16 failed")
   assert(c == 30, "i32 failed")
   assert(d == 40, "i64 failed")
   assert(e == 50, "u8 failed")
   assert(f == 60, "u16 failed")
   assert(g == 70, "u32 failed")
   assert(h == 80, "u64 failed")
   assert(i == 'A', "char failed")
   assert(j == true, "bool failed")
}

fn test_typed_bindings_smoke() {
   def i32 a = 10
   def u16 b = 20
   def bool c = true
   def ?int some = 7
   def ?int absent = nil
   assert(a == 10, "typed i32 binding")
   assert(b == 20, "typed u16 binding")
   assert(c, "typed bool binding")
   if some != nil {
      def int narrowed = some
      assert(narrowed == 7, "optional narrowing in branch")
   } else {
      assert(false, "optional with value should narrow")
   }
   assert(absent == nil, "optional return nil")
}

fn test_null_contracts() {
   def *int p = nil
   assert(p == nil, "typed pointer nil assignment failed")
   def ?int o = nil
   assert(o == nil, "typed nullable int nil assignment failed")
   def ?int q = 9
   assert(q == 9, "typed nullable int value assignment failed")
}

fn test_pointer_arg_types() {
   def *int p = malloc(8)
   assert(p != nil, "typed pointer malloc failed")
   assert(echo_int_ptr(p) == p, "typed pointer arg roundtrip failed")
   write_int_ptr(p, 33)
   assert(read_int_ptr(p) == 33, "typed pointer arg load/store failed")
   assert(echo_int_ptr(nil) == nil, "typed pointer nil arg failed")
   assert(read_int_ptr(nil) == 0, "typed pointer nil read failed")
   assert(write_int_ptr(nil, 44) == nil, "typed pointer nil write failed")
   free(p)
}

fn test_handle_arg_types() {
   def ptr p = malloc(8)
   assert(p != nil, "handle test malloc failed")
   store64_h(p, 0x12345678)
   def handle h = load64_h(p)
   assert(h == 0x12345678, "typed handle load failed")
   assert(echo_handle(h) == h, "typed handle arg/result failed")
   assert(echo_handle(0) == 0, "typed handle accepts zero")
   assert(read_opt_handle(h) == h, "nullable handle value failed")
   assert(read_opt_handle(nil) == 0, "nullable handle nil failed")
   free(p)
}

fn test_raw_i64_buffer_types() {
   def ptr p = malloc(16)
   assert(p != nil, "raw i64 test malloc failed")
   store64_i(p, 7, 0)
   store64_i(p, -3, 8)
   def int a = load64_i(p, 0)
   def int b = load64_i(p, 8)
   assert(a == 7 && b == -3, "raw i64 load/store preserves signed values")
   store64_i(p, a + 1, 0)
   assert(load64_i(p, 0) == 8, "raw i64 load participates in normal int arithmetic")
   free(p)
}

fn test_typed_float_abi() {
   def f64 a = typed_f64_add(1.25, 2.75)
   def f64 b = typed_f32_f64_mix(1.5f32, 2.25)
   def f64 c = typed_f64_tail(2.0, 4.0)
   def f64 d = a > b ? a : b
   def f64 e = a < b ? a : b
   assert(int(a * 100.0) == 400, "typed f64 params/result failed")
   assert(int(b * 100.0) == 375, "typed f32/f64 params failed")
   assert(int(c * 100.0) == 900, "typed f64 implicit tail failed")
   assert(int(d * 100.0) == 400, "typed f64 ternary true arm failed")
   assert(int(e * 100.0) == 375, "typed f64 ternary false arm failed")
   assert(need_int(dynamic_float_size()) == 15, "dynamic float to typed int param failed")
}

fn test_typed_f64_buffer() {
   def b = f64buf_new(4)
   f64buf_store(b, 0, 1.5)
   f64buf_store(b, 1, 2.25)
   f64buf_store(b, 2, 3.0)
   f64buf_store(b, 3, -0.75)
   mut f64 sum = 0.0
   mut i = 0
   while i < 4 {
      sum += f64buf_load(b, i)
      i += 1
   }
   assert(int(sum * 100.0) == 600, "typed f64 buffer load/store failed")
}

fn test_typed_collections() {
   assert(typed_list_size([1, 2, 3]) == 3, "typed list param failed")
   assert(typed_dict_size(set(dict(4), "a", 1)) == 1, "typed dict param failed")
   assert(typed_tuple_size((1, 2, 3)) == 3, "typed tuple param failed")
   mut s = set()
   s = set_mod.add(s, "x")
   assert(typed_set_has_x(s), "typed set param failed")
   assert(s.len == 1, "set len property failed")
   assert(typed_bytes_size(bytes(4)) == 4, "typed bytes param failed")
   assert(typed_list_index_sum([1, 2, 3, 4]) == 10, "typed list index sum failed")
   assert(typed_nested_index_sum([[1, 2, 3], [4, 5, 6]]) == 14, "typed nested list index sum failed")
   assert(nullable_list_index_sum() == 12, "nullable list index sum failed")
}

fn test_runtime_type_shape() {
   def rows = [[1, 2], [3, 4]]
   assert(eq(type(rows), "list"), "type keeps top-level list tag")
   assert(eq(type(rows[0]), "list"), "type keeps nested list tag")
   assert(eq(type(rows[0][0]), "int"), "type reaches scalar after indexing")
   assert(eq(type_shape(rows), "list<list<int>>"), "type_shape nested list")
   assert(eq(type_shape([1, 2, 3]), "list<int>"), "type_shape flat list")
   assert(eq(type_shape([]), "list<empty>"), "type_shape empty list")
   assert(eq(type_shape([1, "x", [2, 3]]), "list<int|str|list<int>>"), "type_shape mixed list")
   assert(eq(type_shape((1, "x", [2, 3])), "list<int|str|list<int>>"), "tuple literal current surface")
   assert(eq(type_shape({"rows": rows}), "dict<str, list<list<int>>>"), "type_shape dict value")
   assert(is_shape(rows, "list<list<int>>"), "is_shape accepts nested list")
   assert(!is_shape(rows, "list<int>"), "is_shape rejects wrong nested list")
   assert(is_shape(rows, ["list<int>", "list<list<int>>"]), "is_shape accepts shape union")
   assert(require_shape(rows, "list<list<int>>")[1][0] == 3, "require_shape returns checked value")
   assert(assert_shape(rows, "list<list<int>>")[0][1] == 2, "assert_shape returns checked value")
   assert(rows.is_shape("list<list<int>>"), "impl any is_shape method")
   assert(eq(rows.require_shape("list<list<int>>").type_shape, "list<list<int>>"), "impl any require_shape method")
   mut s = set()
   s = add(s, "x")
   s = add(s, "y")
   assert(eq(type_shape(s), "set<str>"), "type_shape set element")
   assert(eq(type_shape([[[1]]], 2), "list<list<list>>"), "type_shape max depth")
}

fn test_typeinfer_boolean_results() {
   def int a = 3
   def int b = 5
   def lt = a < b
   def ge = a >= b
   def is_eq = a == b
   assert(eq(type(lt), "bool") && lt, "inferred comparison result stays bool")
   assert(eq(type(ge), "bool") && !ge, "inferred >= result stays bool")
   assert(eq(type(is_eq), "bool") && !is_eq, "inferred == result stays bool")
   def int mask = (a << 2) | (b & 3)
   assert(mask == 13, "integer shift/bitwise inference stays int")
}

fn test_any_dynamic_surface() {
   def any a = 42
   def any b = "ny"
   def any c = [1, 2, 3]
   def any d = {"x": 7}
   assert(echo_any(a) == 42, "any accepts int")
   assert(echo_any(b) == "ny", "any accepts str")
   assert(echo_any(c).len == 3, "any accepts list")
   assert(echo_any(d).get("x", 0) == 7, "any accepts dict")
   assert(any_type_name(true) == "bool", "any parameter accepts bool literal")
   assert(ty.require_type(echo_any([4, 5]), "seq").len == 2, "any result remains shape-checkable")
}

fn test_language_type_groups() {
   assert(ty.normalize_type_name("num") == "number", "type group alias num")
   assert(ty.is_type(42, "number"), "language number group accepts int")
   assert(ty.is_type(3.5, "number"), "language number group accepts float")
   assert(!ty.is_type("42", "number"), "language number group rejects string")
   assert(ty.is_type("abc", "seq"), "language seq group accepts str")
   assert(ty.is_type([1, 2, 3], "sequence"), "language sequence alias accepts list")
   assert(ty.is_type({"x": 1}, "collection"), "language collection group accepts dict")
   ty.define_type_alias("amount", "number")
   ty.define_type_group("math_input", ["amount"])
   assert(ty.is_type(7, "math_input"), "custom type group accepts alias")
   ty.extend_type_group("math_input", ["seq"])
   assert(ty.is_type([1, 2], "math_input"), "custom type group extension")
   assert(ty.require_type(11, "math_input", "math input required") == 11, "require_type accepts group")
   assert(eq(ty.assert_type("xy", "math_input", "math input required"), "xy"), "assert_type accepts group")
}

fn type_group_passthrough(number x) number {
   x
}

impl SelfBox {
   fn value(self box) int {
      box.get("value", 0)
   }
   fn add(self a, self b) self {
      SelfBox({"value": a.value + b.value})
   }
   fn same(self a, self b) bool {
      a.value == b.value
   }
   fn maybe(self box, bool keep) ?self {
      if keep { return box }
      nil
   }
   operator + self: self = add
   operator == self: bool = same
}

layout SelfPair {
   i32 x,
   i32 y
}

impl SelfPair {
   fn new(i32 x, i32 y) *self {
      def ptr out = malloc(__layout_size("SelfPair"))
      store_layout(out, "SelfPair", x, y)
      out
   }
   fn sum(*self p) i32 {
      load32(p, __layout_offset("SelfPair", "x")) + load32(p, __layout_offset("SelfPair", "y"))
   }
}

impl Meter {
   fn val(self m) int {
      m.get("value", 0)
   }
   fn same(self a, self b) bool {
      a.val == b.val
   }
   operator == self: bool = same
}

impl ShapeBox {
   fn value(self b) list {
      b.get("value", [])
   }
   fn shape(self b) str {
      b.value.type_shape
   }
   fn concat(self a, self b) self {
      def left = a.value.require_shape("list<int>", "ShapeBox + left")
      def right = b.value.require_shape("list<int>", "ShapeBox + right")
      ShapeBox({"value": left + right})
   }
   fn same_shape(self a, self b) bool {
      eq(a.shape, b.shape)
   }
   operator + self: self = concat
}

impl int, f32 {
   fn twice(self x) self {
      x + x
   }
}

layout TypePoint {
   i32 x,
   i32 y
}

layout TypePacked pack(1){
   u8 a,
   u32 b
}

layout TypeAligned align(16){
   u8 a
}

layout TypeStorePacked pack(1){
   u8 a,
   u16 b,
   f32 c,
   bool d
}

layout TypeMaterial pack(4){
   i32 base_tex,
   i32 normal_tex,
   i32 flags,
   f64 metallic,
   f64 roughness
}

layout record TypeDerivedMaterial derive(default, eq, hash, debug_str) pack(4){
   i32 base_tex = -1,
   i32 normal_tex = -1,
   i32 flags = 0,
   f64 metallic = 1.0,
   f64 roughness = 0.5
}

layout shape TypeTextureInfo derive(load, store, zero) pack(4){
   i32 index = -1,
   i32 texCoord = 0,
   f64 scale = 1.0,
   bool enabled = true
}

layout shape TypeHeaderInfo derive(load, debug_str) pack(8){
   str sender = "",
   int priority = 0
}

layout shape StrictHeader derive(load) pack(8){
   str sender = "",
   int priority = 0
}

layout shape StrictInferHeader derive(load) pack(8){
   str sender = "",
   int priority = 0
}

layout TypePushConstants pack(4){
   f64 time,
   f64 exposure,
   i32 base_color,
   i32 tex_id,
   i32 flags
}

layout TypeMethodPair pack(4){
   i32 x,
   i32 y,
   fn new(i32 x, i32 y) *self {
      def ptr out = malloc(__layout_size("TypeMethodPair"))
      store_layout(out, "TypeMethodPair", x, y)
      out
   }
   fn sum(*self pair) i32 {
      load32(pair, __layout_offset("TypeMethodPair", "x")) +
      load32(pair, __layout_offset("TypeMethodPair", "y"))
   }
}

layout TypeWindowState {
   i32 backend,
   i32 width,
   i32 height,
   i32 events
}

impl TypeMaterial {
   fn has_texture(*self mat) bool {
      def i32 flags = load32(mat, __layout_offset("TypeMaterial", "flags"))
      (flags & 1) != 0
   }
   fn score(*self mat) f64 {
      def f64 metallic = load64_f64(mat, __layout_offset("TypeMaterial", "metallic"))
      def f64 roughness = load64_f64(mat, __layout_offset("TypeMaterial", "roughness"))
      metallic * (1.0 - roughness)
   }
}

fn test_impl_self_type_alias() {
   def SelfBox a = SelfBox({"value": 5})
   def SelfBox b = SelfBox({"value": 8})
   def SelfBox c = a + b
   assert(c.value == 13, "impl self return type")
   assert(a.add(b).value == 13, "impl self method parameter type")
   assert(a == SelfBox({"value": 5}), "impl self operator type")
   def ?SelfBox kept = a.maybe(true)
   if kept != nil {
      assert(kept.value == 5, "impl nullable self return")
   } else {
      assert(false, "impl nullable self should be present")
   }
   assert(a.maybe(false) == 0, "impl nullable self nil return")
   def *SelfPair p = SelfPair.new(7, 11)
   assert(p.sum() == 18, "impl pointer self receiver")
   assert(SelfPair.sum(p) == 18, "impl pointer self associated call")
   free(p)
}

fn type_material_new(i32 base_tex, i32 normal_tex, f64 metallic, f64 roughness) ptr {
   def ptr mat = malloc(__layout_size("TypeMaterial"))
   store_layout(mat, "TypeMaterial", base_tex, normal_tex, (base_tex >= 0) ? 1 : 0, metallic, roughness)
   mat
}

fn type_pc_write(ptr dst, f64 time, f64 exposure, i32 base_color, i32 tex_id, i32 flags) ptr {
   store_layout(dst, "TypePushConstants", time, exposure, base_color, tex_id, flags)
   dst
}

fn type_packed_write(ptr dst) ptr {
   store_layout(dst, "TypeStorePacked", 7, 0x1234, 1.5, true)
   dst
}

fn type_read_texture_info(any value) ptr {
   layout guard TypeTextureInfo info = value else {
      return TypeTextureInfo()
   }
   info
}

fn type_read_header_info(any value) *TypeHeaderInfo {
   layout guard TypeHeaderInfo info = value else {
      return TypeHeaderInfo()
   }
   if TypeHeaderInfo_load_priority(info) < 0 {
      return TypeHeaderInfo()
   }
   info
}

fn type_header_result(any value) {
   layout guard TypeHeaderInfo info = value else {
      return err("bad header")
   }
   if TypeHeaderInfo_load_priority(info) < 0 {
      return err("negative priority")
   }
   ok(info)
}

fn type_header_result_annotated(any value) Result<*TypeHeaderInfo, str> {
   layout guard TypeHeaderInfo info = value else {
      return err("bad header")
   }
   ok(info)
}

fn strict_result(bool ok_flag) Result<int, str> {
   if ok_flag { return ok(7) }
   err("bad")
}

fn strict_inferred_header_result(any raw) {
   layout guard StrictInferHeader h = raw else {
      return err("bad header")
   }
   ok(h)
}

fn type_win_new(i32 backend, i32 width, i32 height) ptr {
   def ptr state = malloc(__layout_size("TypeWindowState"))
   store_layout(state, "TypeWindowState", backend, width, height, 0)
   state
}

fn type_backend_poll(ptr state) i32 {
   def i32 backend = load32(state, __layout_offset("TypeWindowState", "backend"))
   def i32 events = load32(state, __layout_offset("TypeWindowState", "events")) + 1
   store32(state, events, __layout_offset("TypeWindowState", "events"))
   if backend == 1 { return 10 }
   if backend == 2 { return 11 }
   0
}

fn test_operator_examples() {
   def i = 15
   assert((i + 1) % 16 == 0, "mod inner parens")
   assert(((i + 1) % 16) == 0, "mod outer parens")
   assert(16 % 8 == 0, "mod no parens")
   assert((10 + 5) * 2 % 10 == 0, "complex precedence")
   def a = vec.Vector3(1.0, 2.0, 3.0)
   def b = vec.Vector3(4.0, 5.0, 6.0)
   def sum = a + b
   def f64 dots = a * b
   def half = b / 2.0
   def ratio = b / a
   assert(sum.x == 5.0 && sum.y == 7.0 && sum.z == 9.0, "scoped vec3 add")
   assert(dots == 32.0, "scoped vec3 dot")
   assert(half.x == 2.0 && half.y == 2.5 && half.z == 3.0, "scoped vec3 div")
   assert(ratio.x == 4.0 && ratio.y == 2.5 && ratio.z == 2.0, "scoped vec3 component div")
   assert(a.add(b).z == 9.0, "impl vec3 method call")
   assert(a.dot(a) == 14.0, "impl vec3 dot method")
   assert(vec.runtime_type(sum) == "vec3" && vec.is_vec3(sum), "scoped add preserves vec3")
   assert((2.0 * a).z == 6.0, "imported scalar-left vec3 operator")
   assert(vec.op("/", b, a).x == 4.0, "dynamic vec3 component div")
   def Meter meters_a = Meter(dict(2).set("value", 7))
   def Meter meters_b = Meter(dict(2).set("value", 7))
   assert(meters_a.val == 7, "custom language type property call")
   assert(meters_a == meters_b, "custom language type operator")
   def ShapeBox box_a = ShapeBox({"value": [1, 2]})
   def ShapeBox box_b = ShapeBox({"value": [3, 4]})
   def ShapeBox box_nested = ShapeBox({"value": [[1], [2]]})
   def ShapeBox box_sum = box_a + box_b
   assert(eq(box_a.shape, "list<int>"), "impl method can expose type_shape")
   assert(box_a.same_shape(box_b), "impl method can compare runtime shapes")
   assert(!box_a.same_shape(box_nested), "impl method rejects different runtime shape")
   assert(box_sum.value == [1, 2, 3, 4], "shape-checked custom add operator")
   assert(eq(box_sum.value.type_shape, "list<int>"), "operator result keeps checked shape")
   assert(21.twice() == 42, "multi-owner int impl method")
   def f32 scalar = 1.5f32
   assert(scalar.twice() == 3.0, "multi-owner f32 impl method")
}

fn test_layout_examples() {
   assert(__layout_size("TypePoint") == 8, "TypePoint size")
   assert(__layout_align("TypePoint") == 4, "TypePoint align")
   assert(__layout_offset("TypePoint", "y") == 4, "TypePoint y offset")
   assert(__layout_size("TypePacked") == 5, "TypePacked size")
   assert(__layout_align("TypePacked") == 1, "TypePacked align")
   assert(__layout_offset("TypePacked", "b") == 1, "TypePacked b offset")
   assert(__layout_size("TypeAligned") == 16, "TypeAligned size")
   assert(__layout_align("TypeAligned") == 16, "TypeAligned align")
   assert(__layout_offset("TypeDerivedMaterial", "roughness") == 20, "record roughness offset")
   assert(__layout_offset("TypeTextureInfo", "scale") == 8, "shape scale offset")
   def ptr pair = malloc(__layout_size("TypeMethodPair"))
   store_layout(pair, "TypeMethodPair", 7, 11)
   assert(load32(pair, __layout_offset("TypeMethodPair", "x")) +
   load32(pair, __layout_offset("TypeMethodPair", "y")) == 18, "layout-local pointer fields")
   free(pair)
   def *TypeMaterial mat = type_material_new(4, 7, 0.8, 0.25)
   assert(load32(mat, __layout_offset("TypeMaterial", "base_tex")) == 4, "material base tex")
   assert(load32(mat, __layout_offset("TypeMaterial", "flags")) != 0, "attached material texture flag")
   assert(load64_f64(mat, __layout_offset("TypeMaterial", "metallic")) *
      (1.0 - load64_f64(mat, __layout_offset("TypeMaterial", "roughness"))) == 0.6000000000000001,
   "material score from layout fields")
   free(mat)
   def ptr derived_default = malloc(__layout_size("TypeDerivedMaterial"))
   store_layout(derived_default, "TypeDerivedMaterial", -1, -1, 0, 1.0, 0.5)
   assert(load32(derived_default, __layout_offset("TypeDerivedMaterial", "base_tex")) != 0, "record default int storage")
   assert(load64_f64(derived_default, __layout_offset("TypeDerivedMaterial", "metallic")) == 1.0, "record default f64")
   def ptr derived_a = malloc(__layout_size("TypeDerivedMaterial"))
   def ptr derived_b = malloc(__layout_size("TypeDerivedMaterial"))
   store_layout(derived_a, "TypeDerivedMaterial", 4, 7, 1, 0.8, 0.25)
   store_layout(derived_b, "TypeDerivedMaterial", 4, 7, 1, 0.8, 0.25)
   assert(load32(derived_a, __layout_offset("TypeDerivedMaterial", "base_tex")) ==
   load32(derived_b, __layout_offset("TypeDerivedMaterial", "base_tex")), "record raw field equality")
   assert(load64_f64(derived_a, __layout_offset("TypeDerivedMaterial", "roughness")) ==
   load64_f64(derived_b, __layout_offset("TypeDerivedMaterial", "roughness")), "record raw f64 equality")
   free(derived_default)
   free(derived_a)
   free(derived_b)
   def tex_src = dict(4).set("index", 3).set("texCoord", 1).set("scale", 0.5).set("enabled", false)
   def ptr tex_info = type_read_texture_info(tex_src)
   assert(TypeTextureInfo_load_index(tex_info) == 3, "shape load index")
   assert(TypeTextureInfo_load_texCoord(tex_info) == 1, "shape load tex coord")
   assert(TypeTextureInfo_load_scale(tex_info) == 0.5, "shape load f64")
   assert(TypeTextureInfo_load_enabled(tex_info) == false, "shape load bool")
   TypeTextureInfo_store(tex_info, 9, 2, 0.25, true)
   assert(TypeTextureInfo_load_index(tex_info) == 9, "shape derive store")
   free(tex_info)
   def ptr tex_default = type_read_texture_info(123)
   assert(TypeTextureInfo_load_index(tex_default) == -1, "layout guard fallback")
   assert(TypeTextureInfo_load_enabled(tex_default), "layout guard fallback defaults")
   free(tex_default)
   def ptr tex_zero = TypeTextureInfo_zero()
   assert(TypeTextureInfo_load_enabled(tex_zero) == false, "shape derive zero")
   free(tex_zero)
   def header_src = dict(2).set("sender", "Gemini").set("priority", 10)
   def *TypeHeaderInfo header = type_read_header_info(header_src)
   assert(TypeHeaderInfo_load_sender(header) == "Gemini", "layout guard string field load")
   assert(TypeHeaderInfo_load_priority(header) == 10, "layout guard int field load")
   assert(TypeHeaderInfo_load_sender(header) == "Gemini", "shape load str")
   assert(TypeHeaderInfo_debug_str(header) != "", "shape debug str str")
   free(header)
   match type_header_result(header_src) {
      ok(info) -> {
         assert(TypeHeaderInfo_load_sender(info) == "Gemini", "Result match preserves layout payload str")
         assert(TypeHeaderInfo_load_priority(info) == 10, "Result match preserves layout payload int")
         free(info)
      }
      err(e) -> assert(false, e)
   }
   match type_header_result_annotated(header_src) {
      ok(info) -> {
         assert(TypeHeaderInfo_load_sender(info) == "Gemini", "annotated Result return preserves payload str")
         free(info)
      }
      err(e) -> assert(false, e)
   }
   def *TypeHeaderInfo header_wrapped_ptr = TypeHeaderInfo_from(header_src)
   def header_wrapped = ok(header_wrapped_ptr)
   match header_wrapped {
      ok(info) -> {
         assert(TypeHeaderInfo_load_sender(info) == "Gemini", "ok(binding) preserves layout payload str")
         assert(TypeHeaderInfo_load_priority(info) == 10, "ok(binding) preserves layout payload int")
      }
      err(_) -> assert(false, "unexpected header Err")
   }
   def header_unwrapped = unwrap(header_wrapped)
   assert(TypeHeaderInfo_load_sender(header_unwrapped) == "Gemini", "unwrap(Result<*layout, E>) returns typed layout ptr")
   assert(TypeHeaderInfo_load_priority(header_unwrapped) == 10, "unwrap keeps typed layout ptr metadata")
   free(header_wrapped_ptr)
   def ptr packed = malloc(__layout_size("TypeStorePacked"))
   type_packed_write(packed)
   assert(load8(packed, __layout_offset("TypeStorePacked", "a")) == 7, "load u8")
   assert(load16(packed, __layout_offset("TypeStorePacked", "b")) == 0x1234, "load u16")
   assert(load32_f32(packed, __layout_offset("TypeStorePacked", "c")) == 1.5, "load f32")
   assert(load8(packed, __layout_offset("TypeStorePacked", "d")) != 0, "load bool")
   free(packed)
   def ptr pc = malloc(__layout_size("TypePushConstants"))
   type_pc_write(pc, 1.25, 0.75, 0xff00ff, 7, 1)
   assert(load32(pc, __layout_offset("TypePushConstants", "tex_id")) == 7, "push tex id")
   assert(load64_f64(pc, __layout_offset("TypePushConstants", "exposure")) == 0.75, "push exposure")
   free(pc)
   def ptr win = type_win_new(1, 1280, 720)
   assert(load32(win, __layout_offset("TypeWindowState", "width")) == 1280, "backend width")
   assert(type_backend_poll(win) == 10, "backend dispatch")
   assert(load32(win, __layout_offset("TypeWindowState", "events")) == 1, "shared state updated")
   free(win)
   def ptr backend = contract.make(1, contract.REQUIRED_RENDER_WINDOW | contract.CAP_CURSOR | contract.CAP_CLIPBOARD,
   contract.REQUIRED_RENDER_WINDOW)
   assert(contract.valid(backend), "complete backend contract")
   assert(contract.has(backend, contract.CAP_SURFACE), "contract surface capability")
   free(backend)
}

fn test_strict_types() {
   def any explicit_any = {"sender": "ny", "priority": 1}
   assert(eq(type(explicit_any), "dict"), "explicit any keeps dict runtime type")
   def dict<str, any>: explicit_dict = {"sender": "ny", "priority": 1}
   assert(eq(type(explicit_dict), "dict"), "explicit dict<str, any> keeps dict runtime type")
   layout guard StrictHeader h = {"sender": "ny", "priority": 2} else {
      assert(false, "layout guard should accept mixed boundary data")
   }
   assert(StrictHeader_load_sender(h) == "ny", "layout guard narrows sender")
   assert(StrictHeader_load_priority(h) == 2, "layout guard narrows priority")
   free(h)
   def *StrictHeader h2 = StrictHeader_from({"sender": "nx", "priority": 3})
   assert(StrictHeader_load_sender(h2) == "nx", "Layout_from narrows sender")
   assert(StrictHeader_load_priority(h2) == 3, "Layout_from narrows priority")
   free(h2)
   def r = strict_result(true)
   assert(unwrap(r) == 7, "typed Result unwrap is allowed under strict types")
   match r {
      ok(v) -> assert(v == 7, "typed Result ok payload refines")
      err(e) -> assert(false, e)
   }
}

fn test_flow_null_narrowing() {
   def ?int a = 5
   if a != nil {
      def int v = a
      assert(v == 5, "if x != nil narrowing failed")
      assert(need_int(a) == 5, "call-site narrowing failed")
   }
   def ?int b = 6
   if b == nil {
      assert(false, "unexpected nil in else-narrowing test")
   }else {
      def int w = b
      assert(w == 6, "else branch narrowing failed")
      assert(need_int(b) == 6, "else branch call-site narrowing failed")
   }
   def ?int c = 7
   if nil != c {
      def int x = c
      assert(x == 7, "reversed nil != x narrowing failed")
   }
   def ?int d = 8
   if nil == d {
      assert(false, "unexpected nil in reversed equality narrowing test")
   }else {
      def int y = d
      assert(y == 8, "reversed nil == x else narrowing failed")
   }
   def ?int e = 10
   if e != nil && need_int(e) == 10 {
      assert(true, "logical && rhs narrowing failed")
   }else {
      assert(false, "logical && rhs narrowing branch failed")
   }
   def ?int f = 11
   if f == nil || need_int(f) == 11 {
      assert(true, "logical || rhs narrowing failed")
   }else {
      assert(false, "logical || rhs narrowing branch failed")
   }
   def ?int g = 12
   if g == nil || false {
      assert(false, "logical || else branch narrowing setup failed")
   }else {
      def int z = g
      assert(z == 12, "logical || else branch narrowing failed")
   }
   mut ?int h = 13
   if h != nil {
      h = nil
      assert(h == nil, "mutable nullable assignment after narrowing failed")
   }
   def ?int i = 14
   if (i != nil) && is_nonzero(i) {
      assert(true, "nested logical narrowing failed")
   }else {
      assert(false, "nested logical narrowing branch failed")
   }
   def ?int j = 2
   def ?int k = 3
   if j != nil && k != nil {
      def int sum = need_int(j) + need_int(k)
      assert(sum == 5, "multi-var && branch narrowing failed")
   }else {
      assert(false, "multi-var && branch should be true")
   }
   def ?int m = 4
   def ?int n = 5
   if m == nil || n == nil {
      assert(false, "multi-var || else narrowing setup failed")
   }else {
      def int mv = m
      def int nv = n
      assert(mv + nv == 9, "multi-var || else branch narrowing failed")
   }
}

;; Inference audit: identity preserves argument type.
fn hm_identity(x) {
   x
}

;; Inference audit: numeric operator constrains argument type.
fn hm_add1(x) {
   x + 1
}

;; Inference audit: string operator constrains argument type.
fn hm_suffix(x) {
   x + "!"
}

;; Inference audit: index result follows list element type.
fn hm_first(xs) {
   xs[0]
}

;; Inference audit: nested index result follows inner element type.
fn hm_pick_nested(rows) {
   rows[1][0]
}

;; Inference audit: dictionary lookup result follows value type.
fn hm_getx(d) {
   d["x"]
}

;; Inference audit: generic container parameters preserve element type.
fn hm_sum_typed_f64(list<f64> xs) f64 {
   mut s = 0.0
   mut i = 0
   while i < xs.len {
      s += xs[i]
      i += 1
   }
   s
}

fn hm_mut_float_accumulator(list<f64> xs) f64 {
   mut s = 0.0
   mut i = 0
   while i < xs.len {
      s += xs[i] * 2.0
      i += 1
   }
   s
}

fn hm_update_typed_nested_f64(list<list<f64>> rows) list<list<f64>> {
   def list<f64> row = rows[0]
   row[1] = row[1] + 0.5
   rows[0] = row
   rows
}

fn hm_append_typed_f64() f64 {
   mut list<f64> xs = list(2)
   xs = xs.append(1.5)
   xs = xs.append(-2.5)
   xs[0] + xs[1]
}

fn hm_negative_literal_fold() int {
   -5 + 2
}

fn hm_sum_typed_ints(list<int> xs) int {
   mut s = 0
   mut i = 0
   while i < xs.len {
      s += xs[i]
      i += 1
   }
   s
}

fn hm_pick_typed_nested_int(list<list<int>> rows) int {
   rows[0][1] + rows[1][0]
}

fn hm_pick_typed_nested_f64(list<list<f64>> rows) f64 {
   rows[0][0] + rows[1][1]
}

fn hm_typed_dict_value_sum(dict<str, int> d) int {
   d["x"] + d["y"]
}

;; Inference audit: floating division result.
fn hm_half(x) {
   x / 2.0
}

;; Inference audit: comparison result.
fn hm_small(x) {
   x < 10
}

fn hm_choose_int(bool flag) int {
   if flag { return 21 }
   34
}

fn hm_loop_tail_value() int {
   mut i = 0
   while i < 1 {
      "loop body expression is not a function return"
      i += 1
   }
   7
}

fn hm_nontail_if_value() int {
   if true {
      "non-tail branch expression is not a function return"
   }
   8
}

fn hm_compose(f, g, x) {
   f(g(x))
}

fn hm_map_pair(f, xs) {
   [f(xs[0]), f(xs[1])]
}

fn test_hm_callable_examples() {
   def add_one = fn(x) { x + 1 }
   def shout = fn(str x) str { x + "!" }
   def inferred_num = fn(v) { v + 2 }
   def inferred_str = fn(v) { v + "?" }
   assert(add_one(41) == 42, "HM fn value call")
   assert(shout("ny") == "ny!", "HM typed fn value return")
   assert(inferred_num(5) == 7, "HM lambda infers numeric arg")
   assert(inferred_str("ok") == "ok?", "HM lambda infers string arg")
   assert((fn(str x) str { x + "!" })("go") == "go!", "HM direct fn expression call")
   assert(hm_compose(fn(x) { x + 1 }, fn(x) { x * 2 }, 20) == 41, "HM compose callable chain")
   assert(hm_map_pair(fn(x) { x + 10 }, [1, 2]) == [11, 12], "HM map pair callable")
   def typed_list_lambda = fn(list<int> xs) { xs[0] + xs[1] }
   assert(typed_list_lambda([4, 5]) == 9, "HM lambda generic list parameter")
}

fn test_hm_principal_examples() {
   assert(hm_identity(7) == 7, "HM identity int instantiation")
   assert(hm_identity("ny") == "ny", "HM identity str instantiation")
   def rows = [[1, 2], [3, 4]]
   assert(eq(type_shape(rows), "list<list<int>>"), "HM nested list shape")
   def meta = {"rows": rows}
   assert(eq(type_shape(meta), "dict<str, list<list<int>>>"), "HM dict key/value shape")
   def config = {"sender": "Gemini", "priority": 10, "enabled": true}
   assert(eq(config["sender"], "Gemini"), "HM heterogeneous dict literal str value")
   assert(config["priority"] == 10, "HM heterogeneous dict literal int value")
   assert(config["enabled"], "HM heterogeneous dict literal bool value")
   def config_shape = type_shape(config)
   assert(eq(config_shape, "dict<str, str|int|bool>") ||
      eq(config_shape, "dict<str, str|bool|int>") ||
      eq(config_shape, "dict<str, int|str|bool>") ||
      eq(config_shape, "dict<str, int|bool|str>") ||
      eq(config_shape, "dict<str, bool|str|int>") ||
      eq(config_shape, "dict<str, bool|int|str>"),
   "HM heterogeneous dict runtime shape")
   mut ?int maybe = nil
   if true { maybe = 9 }
   assert(maybe == 9, "HM nullable merge shape")
   assert(hm_choose_int(true) == 21, "HM declared return inference")
   assert(hm_add1(41) == 42, "HM infers numeric operand from + literal")
   assert(hm_suffix("ny") == "ny!", "HM infers string operand from + literal")
   assert(hm_first([9, 8, 7]) == 9, "HM infers list index element int")
   assert(hm_first(["a", "b"]) == "a", "HM infers list index element str")
   assert(hm_first("az") == "a", "HM/runtime string index returns str")
   mut bytes bs = bytes(1)
   bs = bytes_set(bs, 0, 66)
   assert(hm_first(bs) == 66, "HM/runtime bytes index returns int")
   assert(hm_pick_nested([[1], [2]]) == 2, "HM infers nested index element int")
   assert(hm_pick_nested([["a"], ["b"]]) == "b", "HM infers nested index element str")
   assert(hm_getx({"x": 7}) == 7, "HM infers dict index element int")
   assert(hm_getx({"x": "v"}) == "v", "HM infers dict index element str")
   assert(hm_sum_typed_f64([1.0, 2.5, 3.5]) == 7.0, "HM generic list parameter element f64")
   assert(hm_sum_typed_f64([1, 2, 3]) == 6.0, "HM generic list<f64> parameter accepts numeric int elements")
   assert(hm_mut_float_accumulator([1.0, 2.0, 3.0]) == 12.0, "HM mutable f64 accumulator")
   assert(hm_update_typed_nested_f64([[1.0, 2.0], [3.0, 4.0]])[0][1] == 2.5, "HM nested list<f64> update")
   assert(hm_append_typed_f64() == -1.0, "HM typed list<f64> append keeps element type")
   assert(hm_negative_literal_fold() == -3, "unary negative integer literal folds")
   assert(hm_sum_typed_ints([4, 5, 6]) == 15, "HM generic list<int> parameter keeps int element type")
   assert(hm_pick_typed_nested_int([[1, 2], [3, 4]]) == 5, "HM generic nested list<int> parameter")
   assert(hm_pick_typed_nested_f64([[1.5, 2.0], [3.0, 4.5]]) == 6.0, "HM generic nested list<f64> parameter")
   assert(hm_typed_dict_value_sum({"x": 11, "y": 31}) == 42, "HM generic dict<str, int> parameter")
   assert(hm_half(5.0) == 2.5, "HM infers float arithmetic")
   assert(hm_small(3), "HM infers ordered comparison operand")
   assert(hm_loop_tail_value() == 7, "HM loop body final expression is not return")
   assert(hm_nontail_if_value() == 8, "HM non-tail if expression is not return")
}

fn long_runtime_str_source() str { "AB" }

fn long_runtime_hex_source() str { "4142" }

fn long_runtime_any_str_source() any { "AB" }

fn long_runtime_result_source() Result<list<int>, str> { ok([4, 5, 6]) }

fn long_runtime_option_source() ?list<int> { [7, 8, 9] }

fn strict_long_any_source() any { [1, 2, 3] }

fn strict_long_result_source() Result<list<int>, str> { ok([4, 5, 6]) }

fn strict_long_option_source() ?list<int> { [7, 8, 9] }

fn long_runtime_long_return_from_list(list<int> xs) any { xs.long }

fn long_runtime_long_return_from_any(any x) any { x.long }

fn test_strict_type_inference_regressions() {
   assert(strict_bare_list_sum([1, 2, 3]) == 6, "strict bare list infers int elements")
   assert(strict_bare_nested_list_sum([[1, 2, 3], [4, 5, 6]]) == 14, "strict bare nested list infers indexed rows")
   assert(strict_bare_dict_sum({"x": 11, "y": 31}) == 42, "strict bare dict infers indexed values")
   assert(strict_bare_float_list_sum([1.0, 2.5, 3.5]) == 7.0, "strict bare list infers f64 elements")
   def any strict_raw_header = {"sender": "ny", "priority": 7}
   match strict_inferred_header_result(strict_raw_header) {
      ok(h) -> {
         assert(StrictInferHeader_load_sender(h) == "ny", "strict Result ok payload preserves layout str")
         assert(StrictInferHeader_load_priority(h) == 7, "strict Result ok payload preserves layout int")
         free(h)
      }
      err(e) -> assert(false, e)
   }
   def *StrictInferHeader strict_direct_header = StrictInferHeader_from({"sender": "nx", "priority": 9})
   def strict_direct_result = ok(strict_direct_header)
   match strict_direct_result {
      ok(h) -> assert(StrictInferHeader_load_priority(h) == 9, "strict ok(value) keeps known payload with wildcard err arm")
      err(_) -> assert(false, "unexpected err")
   }
   free(strict_direct_header)
}

fn test_strict_long_regressions() {
   static_assert("ABC".long == 0x414243, "strict static string .long")
   static_assert([1, 2, 3].long == 0x010203, "strict static list .long")
   static_assert("010203".unhex.long == 0x010203, "strict static unhex .long")
   static_assert("AAAAAAAAAA".long == Z("308157561862552534729025"), "strict static big .long")
   def int ct = comptime{ return "ABC".long }
   assert(ct == 0x414243, "strict comptime .long")
   def inferred_ct = comptime{ return [1, 2, 3].long }
   assert(inferred_ct == 0x010203, "strict inferred comptime .long keeps integer type")
   def inferred_big_ct = comptime{ return "AAAAAAAAAA".long }
   assert(inferred_big_ct == Z("308157561862552534729025"), "strict inferred comptime big .long")
   assert(type(inferred_big_ct) == "bigint", "strict inferred comptime big .long keeps bigint runtime")
   def any dynamic_bytes = [1, 2, 3]
   assert(to_str(dynamic_bytes.long) == "66051", "strict any receiver allows .long")
   assert(strict_long_any_source().long == Z(0x010203), "strict returned any allows .long")
   def long_chain_bytes = [1, 2, 3].long.bytes
   assert(long_chain_bytes == [1, 2, 3], "strict .long.bytes roundtrip")
   assert("010203".unhex.long.bytes.long == Z(0x010203), "strict .unhex.long.bytes.long roundtrip")
   match strict_long_result_source() {
      ok(v) -> assert(v.long == Z(0x040506), "strict Result payload allows .long")
      err(e) -> assert(false, e)
   }
   def strict_long_maybe = strict_long_option_source()
   if strict_long_maybe != nil {
      assert(strict_long_maybe.long == Z(0x070809), "strict Option payload allows .long")
   }
   assert_compile_range(1, 0, 2, "strict range proof builtin")
   assert_compile_index([10, 20, 30], 1, "strict index proof builtin")
}

fn test_dynamic_byte_list_long_property() {
   static_assert([1, 2, 3].long == 0x010203, "compile-time list .long property")
   static_assert("ABC".long == 0x414243, "compile-time string .long property")
   static_assert("010203".unhex.long == 0x010203, "compile-time unhex .long property")
   static_assert("ABC".to_bytes.long == 0x414243, "compile-time to_bytes .long property")
   static_assert(123.long == 123, "compile-time int .long property")
   static_assert("AAAAAAAAAA".long == [65, 65, 65, 65, 65, 65, 65, 65, 65, 65].long, "compile-time big string/list .long property")
   static_assert("41414141414141414141".unhex.long == "AAAAAAAAAA".long, "compile-time big unhex .long property")
   static_assert("AAAAAAAAAA".long == Z("308157561862552534729025"), "compile-time big .long compares against Z")
   static_assert([1, 2, 3].long == Z("00066051"), "compile-time .long compares against canonical Z")
   def ct_long = comptime{ return [1, 2, 3].long }
   assert(ct_long == 0x010203, "comptime list .long property")
   def ct_unhex_long = comptime{ return "010203".unhex.long }
   assert(ct_unhex_long == 0x010203, "comptime unhex .long property")
   def ct_big_long = comptime{ return "AAAAAAAAAA".long }
   assert(type(ct_big_long) == "bigint", "comptime big .long returns bigint")
   assert(ct_big_long == Z("308157561862552534729025"), "comptime big string .long property")
   def ct_big_unhex_long = comptime{ return "41414141414141414141".unhex.long }
   assert(ct_big_unhex_long == Z("308157561862552534729025"), "comptime big unhex .long property")
   mut built = []
   built = built.append(1)
   built = built.append(2)
   built = built.append(3)
   assert(built.long == Z(0x010203), "dynamic appended list .long property")
   mut list annotated = []
   annotated = annotated.append(4)
   annotated = annotated.append(5)
   assert(annotated.long == Z(0x0405), "annotated list .long property")
   mut from_ctor = list(0)
   from_ctor = from_ctor.append(6)
   from_ctor = from_ctor.append(7)
   assert(from_ctor.long == Z(0x0607), "list constructor .long property")
   def literal = [8, 9]
   assert(literal.long == Z(0x0809), "literal list .long property")
   assert("ABC".long == Z(0x414243), "string .long property")
   assert(long_runtime_str_source().long == Z(0x4142), "runtime string result .long property")
   assert(long_runtime_any_str_source().long == Z(0x4142), "runtime any string result .long property")
   assert(long_runtime_long_return_from_list([1, 2, 3]) == Z(0x010203), ".long returns stable runtime integer value")
   assert(long_runtime_long_return_from_any([1, 2, 3]) == Z(0x010203), "any .long returns stable runtime integer value")
   assert("010203".unhex.long == Z(0x010203), "unhex list .long property")
   assert(long_runtime_hex_source().unhex.long == Z(0x4142), "runtime hex string result .unhex.long property")
   assert(long_runtime_str_source().to_bytes.long == Z(0x4142), "runtime string result .to_bytes.long property")
   def any as_any = [10, 11]
   assert(as_any.long == Z(0x0a0b), "any receiver .long property")
   def any any_hex = "4142"
   assert(any_hex.unhex.long == Z(0x4142), "any string receiver .unhex.long property")
   match long_runtime_result_source() {
      ok(v) -> assert(v.long == Z(0x040506), "result payload .long property")
      err(e) -> assert(false, e)
   }
   def maybe_long_bytes = long_runtime_option_source()
   if maybe_long_bytes != nil {
      assert(maybe_long_bytes.long == Z(0x070809), "option payload .long property")
   }
   mut bytes raw_bytes = bytes(3)
   raw_bytes = bytes_set(raw_bytes, 0, 12)
   raw_bytes = bytes_set(raw_bytes, 1, 13)
   raw_bytes = bytes_set(raw_bytes, 2, 14)
   assert(raw_bytes.long == Z(0x0c0d0e), "native bytes .long property")
   def any raw_as_any = raw_bytes
   assert(raw_as_any.long == Z(0x0c0d0e), "any native bytes .long property")
   def mixed = {"list": [1, 2], "str": "AB", "int": 123}
   assert(mixed["list"].long == Z(0x0102), "heterogeneous dict list .long property")
   assert(mixed["str"].long == Z(0x4142), "heterogeneous dict str .long property")
   assert(mixed["str"].unhex.long == Z(0xab), "heterogeneous dict str .unhex.long property")
   assert(mixed["int"].long == Z(123), "heterogeneous dict int .long property")
   assert(123.long == Z(123), "int .long property")
   assert(1_000.long == Z(1000), "numeric separator int .long property")
   assert(0x01_02_03.long == Z(0x010203), "hex separator int .long property")
   assert(0b1010 == 10, "binary integer literal")
   assert(0B1010 == 10, "uppercase binary integer literal")
   assert(0b1010_1100 == 172, "binary integer literal separators")
   assert(0b1111u8 == 15, "binary integer literal suffix")
   def big_bin_lit = 0b1_00000000_00000000_00000000_00000000_00000000_00000000_00000000_00000000
   assert(type(big_bin_lit) == "bigint", "large binary integer literal promotes to bigint")
   assert(to_str(big_bin_lit) == "18446744073709551616", "large binary integer literal value")
   def max_u64_bin_lit = 0b11111111_11111111_11111111_11111111_11111111_11111111_11111111_11111111
   assert(to_str(max_u64_bin_lit) == "18446744073709551615", "large binary integer literal max u64 value")
   assert(0o77 == 63, "octal integer literal")
   assert(0o1_234 == 668, "octal integer literal separators")
   assert(int(Z(3)) == 3, "int(bigint) converts runtime BigInt to tagged int")
   assert(eq(type_shape(int(Z(3))), "int"), "int(bigint) keeps static int shape")
   assert(int(next_prime(2)) == 3, "int(next_prime) converts BigInt helper output")
   assert(int("not numeric") == 0, "int(non numeric) returns tagged zero")
   assert(Z(456).long == Z(456), "bigint .long property")
   assert(1.0.long == Z(1), "float .long property")
   assert(1.9.long == Z(1), "float .long truncates toward zero")
   assert((-1.9).long == Z(-1), "negative float .long truncates toward zero")
   def any float_as_any = 12.75
   assert(float_as_any.long == Z(12), "any float .long property")
   assert("ABC".to_bytes.long == Z(0x414243), "chained to_bytes .long property")
   def z_bytes = Z(0x010203).bytes
   assert(eq(type_shape(z_bytes), "list<int>"), "bigint .bytes exposes typed byte list")
   assert(z_bytes[1] == 2, "bigint .bytes keeps typed index")
   assert(z_bytes.long == Z(0x010203), "bigint .bytes roundtrips through .long")
   def abc_bytes = Z(0x414243).bytes
   assert(eq(type_shape(abc_bytes), "list<int>"), "bigint .bytes preserves typed list for extension methods")
   assert(abc_bytes.text == "ABC", "list<int> dispatches impl list extension method")
   def i_bytes = int_to_bytes(0x040506)
   assert(eq(type_shape(i_bytes), "list<int>"), "int_to_bytes exposes typed byte list")
   assert(i_bytes[2] == 6, "int_to_bytes keeps typed index")
   assert(i_bytes.long == Z(0x040506), "int_to_bytes roundtrips through .long")
   assert(int_to_bytes(0x414243).text == "ABC", "list<int> extension method works on int_to_bytes")
   def long_chain_bytes = [1, 2, 3].long.bytes
   assert(eq(type_shape(long_chain_bytes), "list<int>"), ".long.bytes exposes typed byte list")
   assert(long_chain_bytes == [1, 2, 3], ".long.bytes roundtrips literal list")
   assert("010203".unhex.long.bytes[2] == 3, ".unhex.long.bytes keeps typed index")
   assert("010203".unhex.long.bytes.long == Z(0x010203), ".unhex.long.bytes roundtrips through .long")
   def s_bytes = "ABC".to_bytes
   assert(eq(type_shape(s_bytes), "list<int>"), "str .to_bytes exposes typed byte list")
   assert(s_bytes[0] == 65, "str .to_bytes keeps typed index")
   def hex_bytes = "010203".unhex
   assert(eq(type_shape(hex_bytes), "list<int>"), "str .unhex exposes typed byte list")
   assert(hex_bytes[2] == 3, "str .unhex keeps typed index")
}

def type_flow_values = [10, 20, 30]
def int type_flow_idx = 1
assert_compile_range(type_flow_idx, 1, 1, "type flow range proof")
assert_compile_index(type_flow_values, type_flow_idx, "type flow index proof")

fn test_type_flow_range_examples() int {
   def xs = [10, 20, 30]
   mut int i = 0
   mut int acc = 0
   while i < xs.len {
      assert_compile_range(i, 0, 2, "type flow loop range proof")
      assert_compile_index(xs, i, "type flow loop index proof")
      acc += xs[i]
      i += 1
   }
   acc
}

;; Inference audit: masked arithmetic stays monomorphic.
fn mono_mask_mix(a, b) {
   ((a * 1664525) + b + 1013904223) & 0x7fffffff
}

fn test_mono_masked_wrap_examples() int {
   mut int acc = 1
   mut int i = 0
   while i < 64 {
      acc = mono_mask_mix(acc, i)
      i += 1
   }
   acc
}

print("test_add(10, 20) =", test_add(10, 20))
assert(test_add(10, 20) == 30, "add failed")
assert(get_name() == "John", "get_name failed")
assert(process("hello") == "hello", "process failed")
assert(maybe_int(true) == 7, "nullable return value failed")
assert(maybe_int(false) == 0, "nullable nil return failed")
assert(read_opt(nil) == 0, "nullable param nil failed")
assert(read_opt(5) == 5, "nullable param value failed")
assert(nullable_list_after_guard() == 2, "post nil-guard ?list narrowing failed")
assert(nullable_list_after_reversed_guard() == 3, "post reversed nil-guard ?list narrowing failed")
assert(nullable_list_after_else_guard() == 1, "post else-return ?list narrowing failed")
test_primitives()
test_typed_bindings_smoke()
test_null_contracts()
test_pointer_arg_types()
test_handle_arg_types()
test_raw_i64_buffer_types()
test_typed_float_abi()
test_typed_f64_buffer()
test_typed_collections()
test_runtime_type_shape()
test_typeinfer_boolean_results()
test_any_dynamic_surface()
test_language_type_groups()
assert(type_group_passthrough(9) == 9, "language type group annotation")
test_impl_self_type_alias()
test_operator_examples()
test_layout_examples()
test_strict_types()
test_flow_null_narrowing()
test_hm_callable_examples()
test_hm_principal_examples()
test_strict_type_inference_regressions()
test_strict_long_regressions()
test_dynamic_byte_list_long_property()
assert(test_type_flow_range_examples() == 60, "type flow range/index examples")
assert(test_mono_masked_wrap_examples() == 755010081, "mono masked wrap arithmetic")
assert(eq(type(clone((1, 2, 3))), "list"), "tuple clone current surface")
assert(eq(type(sorted((3, 1, 2))), "list"), "tuple sorted current surface")
assert(eq(type(take((1, 2, 3), 2)), "list"), "tuple take current surface")
print("✓ all runtime type tests passed")
