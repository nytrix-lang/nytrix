use std.core
use std.core.error
use std.core.reflect
use std.core.dict
use std.core.io
use std.core.str

;; comptime (Test)
mut v1 = comptime{ return 1 + 2 + 3 }
assert(v1 == 6, "comptime basic")
mut v2 = comptime{ def x = 10 }
assert(v2 == 0, "comptime fallthrough")
mut v3 = comptime{
   mut sum = 0
   mut i = 0
   while(i < 5){
      sum = sum + i
      i += 1
   }
   if(sum == 10){ return sum }
   return 0
}

assert(v3 == 10, "comptime control flow")
mut v4 = comptime{
   def inner = comptime{ return 5 }
   return inner * 2
}

assert(v4 == 10, "comptime nested")
def ct_list = comptime{ [1, 2, 4, 8] }
assert_eq(to_str(ct_list), "[1, 2, 4, 8]", "comptime list materializes")
assert_eq(ct_list[2], 4, "comptime list index")
def ct_words = comptime{ ["a", "b"] }
assert_eq(to_str(ct_words), "[a, b]", "comptime string list materializes")
def ct_range = comptime{ range(4) }
assert_eq(type(ct_range), "range", "comptime imported range type")
assert_eq(ct_range.len, 4, "comptime imported range length")
def ct_mapped = comptime{ range(4).map(fn(i) { i + 1 }) }
assert_eq(to_str(ct_mapped), "[1, 2, 3, 4]", "comptime imports std range map")
def ct_base = comptime{ 2^5 }
def ct_shifted = comptime{ range(4).map(fn(i) { i + ct_base }) }
assert_eq(to_str(ct_shifted), "[32, 33, 34, 35]", "comptime folds prior immutable constant")
print("✓ comptime tests passed")

;; Test basic integer operations in comptime
;; Addition
mut v_add = comptime{ return 10 + 20 }
assert(v_add == 30, "comptime add")

;; Subtraction
mut v_sub = comptime{ return 50 - 20 }
assert(v_sub == 30, "comptime sub")

;; Multiplication
mut v_mul = comptime{ return 6 * 5 }
assert(v_mul == 30, "comptime mul")

;; Division
mut v_div = comptime{ return 60 / 2 }
assert(v_div == 30, "comptime div")

;; Modulo
mut v_mod = comptime{ return 35 % 32 }
assert(v_mod == 3, "comptime mod")

;; Comparisons
mut v_lt = comptime{ return 10 < 20 }
assert(v_lt == true, "comptime lt")
mut v_le = comptime{ return 20 <= 20 }
assert(v_le == true, "comptime le")
mut v_gt = comptime{ return 30 > 20 }
assert(v_gt == true, "comptime gt")
mut v_ge = comptime{ return 20 >= 20 }
assert(v_ge == true, "comptime ge")
mut ct_os = "unknown"
#linux { ct_os = "linux" }
#elif macos { ct_os = "macos" }
#elif windows { ct_os = "windows" }
#endif
assert(ct_os == __os_name(), "comptime #if os select")
mut ct_pick = comptime{
   #linux { return 7 }
   #else { return 9 }
   #endif
}

#linux { assert(ct_pick == 7, "comptime #if nested") }
#else { assert(ct_pick == 9, "comptime #if nested") }
#endif
mut v_guard_div0 = comptime{
   def d = [0][0]
   if(d == 0){ return -99 }
   return 100 / d
}

assert(v_guard_div0 == -99, "comptime guarded div0")
mut v_guard_mod0 = comptime{
   def d = [0][0]
   if(d == 0){ return 321 }
   return 100 % d
}

assert(v_guard_mod0 == 321, "comptime guarded mod0")
static_assert((3 * 7) == 21, "static_assert folded arithmetic")
static_assert(comptime{ return 2 * 21 == 42 }, "static_assert comptime block")
assert_compile((4 * 11) == 44, "assert_compile folded arithmetic")
def ct_range_values = [10, 20, 30]
def int: ct_range_idx = 1
assert_compile(range_proven(ct_range_idx, 1, 1), "range_proven exact binding")
assert_compile(index_proven(ct_range_values, ct_range_idx), "index_proven static list")
assert_compile_range(ct_range_idx + 1, 2, 2, "assert_compile_range expression")
assert_compile_index(ct_range_values, ct_range_idx, "assert_compile_index static list")

fn static_range_sum() int {
   def xs = [1, 2, 3, 4]
   mut int: i = 0
   mut int: acc = 0
   while(i < xs.len){
      assert_compile_range(i, 0, 3, "loop index range proof")
      assert_compile_index(xs, i, "loop index bounds proof")
      acc += xs[i]
      i += 1
   }
   acc
}

assert(static_range_sum() == 10, "compile-time loop range assertions")

comptime diagnostic rule bad_layout_store {
   when call.name == "store_layout" && !is_literal(call.arg(1))
   error "store_layout needs a string literal layout name"
   fix "use store_layout(dst, \"LayoutName\", ...)"
}

fn static_branch_select() int {
   if(comptime{ return true }){
      return 11
   } else {
      return missing_static_branch_symbol()
   }
}

assert(static_branch_select() == 11, "comptime if prunes dead branch")

fn static_case_select() int {
   case comptime{ return 2 } {
      1 -> { return missing_static_case_symbol() }
      2 -> { return 22 }
      _ -> { return missing_static_case_default() }
   }
}

assert(static_case_select() == 22, "comptime case emits selected arm only")

fn static_case_range_select() int {
   case comptime{ return 0xffc1 } {
      0x30..0x39 -> { return missing_static_range_digit() }
      0xffbe..0xffc9 -> { return 33 }
      _ -> { return missing_static_range_default() }
   }
}

assert(static_case_range_select() == 33, "comptime case range emits selected arm only")

layout CtReflectRecord {
   a: i32
   b: f64
   c: bool
}

mut reflect_field_count = 0
mut reflect_field_index_sum = 0
mut reflect_field_type_hits = 0

comptime fields(CtReflectRecord) as f {
   emit assert(__layout_offset("CtReflectRecord", f.name) == f.offset, "comptime fields offset")
   emit reflect_field_count += 1
   emit reflect_field_index_sum += f.index
   emit if(f.type == "i32" || f.type == "f64" || f.type == "bool"){
      reflect_field_type_hits += 1
   }
}

assert(reflect_field_count == 3, "comptime fields count")
assert(reflect_field_index_sum == 3, "comptime fields index sum")
assert(reflect_field_type_hits == 3, "comptime fields type names")

module CtReflectExports(alpha, beta){
   fn alpha() int { return 1 }
   fn beta() int { return 2 }
}

mut reflect_export_count = 0
mut reflect_export_seen = 0

comptime exports(CtReflectExports) as name {
   emit reflect_export_count += 1
   emit if(name == "alpha"){ reflect_export_seen += 1 }
   emit if(name == "beta"){ reflect_export_seen += 10 }
}

assert(reflect_export_count == 2, "comptime exports count")
assert(reflect_export_seen == 11, "comptime exports names")

comptime template make_axis_family(axis){
   fn ct_axis_${axis}() {
      "Generated comptime axis family test."
      return axis
   }
}

for axis in comptime ["x", "y", "z"] {
   emit make_axis_family(axis)
}

assert(ct_axis_x() == "x", "comptime function family string splice x")
assert(ct_axis_y() == "y", "comptime function family string splice y")
assert(ct_axis_z() == "z", "comptime function family string splice z")

comptime template make_mul_family(n){
   fn ct_mul_${n}(int v) int {
      "Generated comptime multiply family test."
      return v * n
   }
}

for n in comptime [2, 3, 5] {
   emit make_mul_family(n)
}

assert(ct_mul_2(7) == 14, "comptime function family int splice 2")
assert(ct_mul_3(7) == 21, "comptime function family int splice 3")
assert(ct_mul_5(7) == 35, "comptime function family int splice 5")

comptime template clamp_num(T, name){
   fn name(T v, T lo, T hi) T {
      if(v < lo){ return lo }
      if(v > hi){ return hi }
      return v
   }
}

comptime emit clamp_num(int, ct_clamp_int)
comptime emit clamp_num(f64, ct_clamp_f64)
assert(ct_clamp_int(-5, 0, 9) == 0, "comptime template symbol fn/type lower bound")
assert(ct_clamp_int(12, 0, 9) == 9, "comptime template symbol fn/type upper bound")
assert(ct_clamp_int(7, 0, 9) == 7, "comptime template symbol fn/type inside")
assert(ct_clamp_f64(-1.5, 0.25, 2.0) == 0.25, "comptime template f64 type lower bound")
assert(ct_clamp_f64(1.5, 0.25, 2.0) == 1.5, "comptime template f64 type inside")

comptime template make_named_suffix(T, name){
   fn ${name}_twice(T v) T {
      return v + v
   }
}

comptime emit make_named_suffix(int, ct_named)
assert(ct_named_twice(6) == 12, "comptime template spliced suffix fn name")

comptime template backend_contract_impl(Contract){
   fn gen_${native_prefix}_backend_name() {
      return native_prefix
   }
   fn gen_${native_prefix}_${Contract}_score() int {
      return event_table + key_table
   }
}

module CtGeneratedBackend generated from CtBackendSpec {
   export core(gen_ctgen_backend_name, gen_ctgen_CtWindowContract_score, manual_backend_code)
   native_prefix = "ctgen"
   event_table = 40
   key_table = 2
   fn manual_backend_code() int {
      7
   }
   emit backend_contract_impl(CtWindowContract)
}

assert(CtGeneratedBackend.gen_ctgen_backend_name() == "ctgen", "generated module string property")
assert(CtGeneratedBackend.gen_ctgen_CtWindowContract_score() == 42, "generated module table properties")
assert(CtGeneratedBackend.manual_backend_code() == 7, "generated module handwritten escape hatch")
print("✓ comptime ops tests passed")
