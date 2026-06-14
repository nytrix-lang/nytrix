;; Keywords: syntax type core
;; Runtime type utilities for Nytrix.
;;
;; The compiler still needs a small amount of primitive type knowledge for code
;; generation, but public runtime grouping is intentionally data-driven here:
;; aliases such as `num -> number` and groups such as `number = [int, float,
;; bigint]` are normal Ny values and can be extended by user code.
;; References:
;; - std.core.syntax
;; - std.core
module std.core.syntax.type(
   is_int, is_float, is_number, is_bool, is_nil,
   is_list, is_dict, is_str, is_tuple, is_set,
   is_bigint, is_poly, is_matrix,
   type_tag, type_name, normalize_type_name, define_type_alias, type_alias,
   define_type_group, extend_type_group, type_group, type_group_members,
   type_group_names, type_groups, is_type_group, is_type, is_one_of,
   require_type, assert_type, require_one_of, assert_one_of,
   register_type, is_registered_type
)

use std.core
use std.core.error
use std.core.primitives as prim
use std.core.str (join)

def TAG_NIL     = 0
def TAG_INT     = 1
def TAG_FFI_PTR = prim.runtime_tag_raw("ffi_ptr")
def TAG_LIST    = prim.runtime_tag_raw("list")
def TAG_DICT    = prim.runtime_tag_raw("dict")
def TAG_SET     = prim.runtime_tag_raw("set")
def TAG_TUPLE   = prim.runtime_tag_raw("tuple")
def TAG_RANGE   = prim.runtime_tag_raw("range")
def TAG_CLOSURE = prim.runtime_tag_raw("ptr")
def TAG_FLOAT   = prim.runtime_tag_raw("float")
def TAG_COMPLEX = prim.runtime_tag_raw("complex")
def TAG_STR1    = prim.runtime_tag_raw("str")
def TAG_STR2    = prim.runtime_tag_raw("str_const")
def TAG_BYTES   = prim.runtime_tag_raw("bytes")
def TAG_BIGINT  = prim.runtime_tag_raw("bigint")
def TAG_POLY    = 302
def TAG_MATRIX  = 303
mut TYPE_NAMES = nil
mut TYPE_ALIASES = nil
mut TYPE_GROUPS = nil
mut TYPE_GROUP_ORDER = nil

fn _name_in_list(any xs, str name) bool {
   if !(is_list(xs) || is_tuple(xs)) { return false }
   mut i = 0
   while i < xs.len {
      if xs.get(i) == name { return true }
      i += 1
   }
   false
}

fn _clone_spec_list(any members) list {
   mut out = list(0)
   if members == nil { return out }
   if is_list(members) || is_tuple(members) {
      mut i = 0
      while i < members.len {
         out = out.append(members.get(i, nil))
         i += 1
      }
      return out
   }
   out.append(members)
}

fn _type_name_set(int tag, str name) str {
   TYPE_NAMES = TYPE_NAMES.set(tag, name)
   name
}

fn _type_alias_set(str name, str target) str {
   TYPE_ALIASES = TYPE_ALIASES.set(name, target)
   target
}

fn _type_group_set(str name, any members) list {
   def xs = _clone_spec_list(members)
   TYPE_GROUPS = TYPE_GROUPS.set(name, xs)
   if !_name_in_list(TYPE_GROUP_ORDER, name) { TYPE_GROUP_ORDER = TYPE_GROUP_ORDER.append(name) }
   xs
}

fn _types_init() int {
   if TYPE_ALIASES != nil { return 0 }
   TYPE_NAMES = dict(32)
   TYPE_ALIASES = dict(64)
   TYPE_GROUPS = dict(32)
   TYPE_GROUP_ORDER = list(0)
   _type_name_set(TAG_NIL, "nil")
   _type_name_set(TAG_INT, "int")
   _type_name_set(TAG_FFI_PTR, "ffi_ptr")
   _type_name_set(TAG_LIST, "list")
   _type_name_set(TAG_DICT, "dict")
   _type_name_set(TAG_SET, "set")
   _type_name_set(TAG_TUPLE, "tuple")
   _type_name_set(TAG_RANGE, "range")
   _type_name_set(TAG_CLOSURE, "ptr")
   _type_name_set(TAG_FLOAT, "float")
   _type_name_set(TAG_COMPLEX, "complex")
   _type_name_set(TAG_STR1, "str")
   _type_name_set(TAG_STR2, "str")
   _type_name_set(TAG_BYTES, "bytes")
   _type_name_set(TAG_BIGINT, "bigint")
   _type_name_set(TAG_POLY, "poly")
   _type_name_set(TAG_MATRIX, "matrix")
   _type_alias_set("none", "nil")
   _type_alias_set("string", "str")
   _type_alias_set("byte_string", "bytes")
   _type_alias_set("BigInt", "bigint")
   _type_alias_set("Poly", "poly")
   _type_alias_set("Matrix", "matrix")
   _type_alias_set("num", "number")
   _type_alias_set("numeric", "number")
   _type_alias_set("sequence", "seq")
   _type_alias_set("function", "fnptr")
   _type_alias_set("i8", "int")
   _type_alias_set("i16", "int")
   _type_alias_set("i32", "int")
   _type_alias_set("i64", "int")
   _type_alias_set("i128", "int")
   _type_alias_set("u8", "int")
   _type_alias_set("u16", "int")
   _type_alias_set("u32", "int")
   _type_alias_set("u64", "int")
   _type_alias_set("u128", "int")
   _type_alias_set("char", "int")
   _type_alias_set("handle", "int")
   _type_alias_set("f32", "float")
   _type_alias_set("f64", "float")
   _type_alias_set("f128", "float")
   _type_group_set("number", ["int", "float", "bigint"])
   _type_group_set("scalar", ["number", "bool", "str"])
   _type_group_set("seq", ["list", "tuple", "str", "range"])
   _type_group_set("collection", ["list", "dict", "set", "tuple"])
   _type_group_set("container", ["collection", "bytes", "range"])
   _type_group_set("iterable", ["seq", "set", "dict", "bytes"])
   _type_group_set("indexable", ["seq", "dict", "bytes"])
   0
}

fn normalize_type_name(str name) str {
   "Returns the canonical runtime type/group name after resolving aliases."
   _types_init()
   mut cur = name
   mut i = 0
   while i < 16 {
      def next = TYPE_ALIASES.get(cur, nil)
      if next == nil || next == cur { return cur }
      cur = next
      i += 1
   }
   cur
}

fn define_type_alias(str name, str target) str {
   "Defines `name` as an alias for `target` and returns the canonical target."
   _types_init()
   def canonical = normalize_type_name(target)
   _type_alias_set(name, canonical)
}

fn type_alias(str name, str target) str {
   "Alias for define_type_alias."
   define_type_alias(name, target)
}

fn _canonical_spec_list(any members) list {
   def xs = _clone_spec_list(members)
   mut out = list(xs.len)
   mut i = 0
   while i < xs.len {
      def item = xs.get(i, nil)
      if is_str(item) {
         out = out.append(normalize_type_name(to_str(item)))
      } else {
         out = out.append(item)
      }
      i += 1
   }
   out
}

fn define_type_group(str name, any members) list {
   "Defines a runtime type group. Members can be type names, tags, aliases, or other groups."
   _types_init()
   _type_group_set(normalize_type_name(name), _canonical_spec_list(members))
}

fn extend_type_group(str name, any members) list {
   "Adds members to a runtime type group without duplicating existing entries."
   _types_init()
   def key = normalize_type_name(name)
   mut out = TYPE_GROUPS.get(key, list(0))
   def add = _canonical_spec_list(members)
   mut i = 0
   while i < add.len {
      def item = add.get(i, nil)
      mut exists = false
      mut j = 0
      while j < out.len {
         if out.get(j, nil) == item { exists = true }
         j += 1
      }
      if !exists { out = out.append(item) }
      i += 1
   }
   _type_group_set(key, out)
}

fn type_group_members(str name) list {
   "Returns a copy of the members for type group `name`, or an empty list."
   _types_init()
   def members = TYPE_GROUPS.get(normalize_type_name(name), nil)
   _clone_spec_list(members)
}

fn type_group(str name) list {
   "Alias for type_group_members."
   type_group_members(name)
}

fn type_group_names() list {
   "Returns known type group names in registration order."
   _types_init()
   _clone_spec_list(TYPE_GROUP_ORDER)
}

fn type_groups() dict {
   "Returns a shallow copy of the type-group registry."
   _types_init()
   dict_clone(TYPE_GROUPS)
}

fn is_type_group(str name) bool {
   "Returns true when `name` resolves to a registered type group."
   _types_init()
   TYPE_GROUPS.get(normalize_type_name(name), nil) != nil
}

fn is_int(any x) bool {
   "Check if x is an integer(tagged pointer with LSB=1)."
   (__tagof(x) & 1) != 0
}

fn is_float(any x) bool {
   "Check if x is a boxed f64 float."
   __is_float_obj(x)
}

fn is_bool(any x) bool {
   "Check if x is a boolean."
   x == true || x == false
}

fn is_nil(any x) bool {
   "Check if x is nil. Integer 0 is NOT nil."
   if __is_int(x) { return false }
   x == nil
}

fn is_list(any x) bool {
   "Check if x is a list."
   __tagof(x) == TAG_LIST
}

fn is_dict(any x) bool {
   "Check if x is a dict."
   __tagof(x) == TAG_DICT
}

fn is_str(any x) bool {
   "Check if x is a string."
   def tag = __tagof(x)
   tag == TAG_STR1 || tag == TAG_STR2
}

fn is_tuple(any x) bool {
   "Check if x is a tuple."
   __tagof(x) == TAG_TUPLE
}

fn is_set(any x) bool {
   "Check if x is a set."
   __tagof(x) == TAG_SET
}

fn is_bigint(any x) bool {
   "Check if x is a BigInt."
   def f = globals().get("std.math.nt.is_bigint")
   if f { return f(x) == true }
   __tagof(x) == TAG_BIGINT
}

fn is_poly(any x) bool {
   "Check if x is a Polynomial(tag 302)."
   def f = globals().get("std.math.crypto.poly.is_poly")
   if f { return f(x) == true }
   __tagof(x) == TAG_POLY
}

fn is_matrix(any x) bool {
   "Check if x is a Matrix(tag 303)."
   def f = globals().get("std.math.matrix.is_matrix")
   if f { return f(x) == true }
   __tagof(x) == TAG_MATRIX
}

fn is_number(any x) bool {
   "Check if x is a member of the language-defined `number` group."
   is_type(x, "number")
}

fn type_tag(any x) int {
   "Get the runtime tag of x."
   __tagof(x)
}

fn type_name(any x) str {
   "Get the canonical runtime type name for x."
   _types_init()
   if is_nil(x) { return "nil" }
   if is_int(x) { return "int" }
   if is_bool(x) { return "bool" }
   if is_float(x) { return "float" }
   def tag = __tagof(x)
   def name = TYPE_NAMES.get(tag, nil)
   if name != nil { return name }
   if !x { return "nil" }
   f"unknown({tag})"
}

fn _is_leaf_type(any x, str name) bool {
   case name {
      "any" -> true
      "int" -> is_int(x)
      "float" -> is_float(x)
      "complex" -> __tagof(x) == TAG_COMPLEX
      "bool" -> is_bool(x)
      "nil" -> is_nil(x)
      "list" -> is_list(x)
      "dict" -> is_dict(x)
      "set" -> is_set(x)
      "tuple" -> is_tuple(x)
      "range" -> __tagof(x) == TAG_RANGE
      "bytes" -> __tagof(x) == TAG_BYTES
      "str" -> is_str(x)
      "ptr" -> is_ptr(x)
      "ffi_ptr" -> __tagof(x) == TAG_FFI_PTR
      "bigint" -> is_bigint(x)
      "poly" -> is_poly(x)
      "matrix" -> is_matrix(x)
      _ -> is_registered_type(x, name)
   }
}

fn _is_type_spec_at(any x, any spec, int depth) bool {
   if depth <= 0 { return false }
   if is_str(spec) { return _type_name_accepts_at(x, to_str(spec), depth) }
   if is_int(spec) { return type_tag(x) == spec }
   if is_list(spec) || is_tuple(spec) { return _any_type_spec_at(x, spec, depth - 1) }
   false
}

fn _any_type_spec_at(any x, any specs, int depth) bool {
   mut i = 0
   while i < specs.len {
      if _is_type_spec_at(x, specs.get(i, nil), depth) { return true }
      i += 1
   }
   false
}

fn _type_name_accepts_at(any x, str name, int depth) bool {
   if depth <= 0 { return false }
   _types_init()
   def n = normalize_type_name(name)
   def group = TYPE_GROUPS.get(n, nil)
   if group != nil { return _any_type_spec_at(x, group, depth - 1) }
   _is_leaf_type(x, n)
}

fn is_type(any x, any spec) bool {
   "Returns true when x matches a type spec.
   Specs can be canonical type names, aliases, group names, numeric tags, or
   lists/tuples of any of those. Built-in groups such as `number` and `seq`
   are ordinary language-level groups and can be extended or replaced."
   _is_type_spec_at(x, spec, 16)
}

fn is_one_of(any x, any spec) bool {
   "Alias for is_type with list/tuple union specs."
   is_type(x, spec)
}

fn _type_spec_to_str(any spec) str {
   if is_list(spec) || is_tuple(spec) {
      mut parts = list(0)
      mut i = 0
      while i < spec.len {
         parts = parts.append(_type_spec_to_str(spec.get(i, nil)))
         i += 1
      }
      return "[" + join(parts, ", ") + "]"
   }
   to_str(spec)
}

fn require_type(any x, any spec, str msg="type check failed") any {
   "Return x if it matches spec, otherwise panic with msg."
   if !is_type(x, spec) { panic(msg) }
   x
}

fn assert_type(any x, any spec, str msg="type check failed") any {
   "Assert that x matches a type spec, panic with expected and actual type names."
   if !is_type(x, spec) { panic(f"{msg}: expected {_type_spec_to_str(spec)}, got {type_name(x)}") }
   x
}

fn require_one_of(any x, any spec, str msg="type check failed") any {
   "Return x if it matches spec, otherwise panic with msg."
   require_type(x, spec, msg)
}

fn assert_one_of(any x, any spec, str msg="type check failed") any {
   "Assert that x matches a type spec, panic with expected and actual type names."
   assert_type(x, spec, msg)
}

mut CUSTOM_TYPES = dict(0)
mut NEXT_CUSTOM_TAG = 200

fn register_type(str name) int {
   "Register a new custom type name, returns assigned tag."
   def existing = CUSTOM_TYPES.get(name, nil)
   if existing != nil { return existing }
   def tag = NEXT_CUSTOM_TAG
   CUSTOM_TYPES = CUSTOM_TYPES.set(name, tag)
   NEXT_CUSTOM_TAG += 1
   if NEXT_CUSTOM_TAG > 299 { panic("Too many custom types(max 100)") }
   tag
}

fn is_registered_type(any x, str name) bool {
   "Check if x is of registered custom type name."
   def tag = CUSTOM_TYPES.get(name, nil)
   if tag == nil { return false }
   __tagof(x) == tag
}

#main {
   assert(is_int(42), "syntax type is_int")
   assert(is_float(3.14), "syntax type is_float")
   assert(is_bool(true), "syntax type is_bool")
   assert(is_list([1, 2, 3]), "syntax type is_list")
   assert(is_dict({"a": 1}), "syntax type is_dict")
   assert(is_str("hello"), "syntax type is_str")
   assert(type_name(42) == "int", "syntax type_name int")
   assert(normalize_type_name("num") == "number", "syntax type alias")
   assert(is_type(42, "number"), "syntax number accepts int")
   assert(is_type(3.14, "number"), "syntax number accepts float")
   assert(!is_type("42", "number"), "syntax number rejects str")
   assert(is_type("abc", "seq"), "syntax seq accepts str")
   assert(type_group_members("number").len >= 3, "syntax group members")
   define_type_alias("text", "str")
   define_type_group("textish", ["text"])
   assert(is_type("abc", "textish"), "syntax custom group alias")
   extend_type_group("textish", ["list"])
   assert(is_type(["a"], "textish"), "syntax custom group extension")
   assert(require_type(42, "number", "must be number") == 42, "syntax require_type")
   assert(assert_type([1, 2, 3], "seq", "must be seq").len == 3, "syntax assert_type")
   def tag = register_type("SelfTestType")
   assert(tag >= 200, "syntax custom type tag")
   assert(register_type("SelfTestType") == tag, "syntax custom type stable")
   print("✓ std.core.syntax.type self-test passed")
}
