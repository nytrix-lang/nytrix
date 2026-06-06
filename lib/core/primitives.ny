;; Keywords: core primitives values types builtins
;; Primitive platform constants, low-level numeric types, and built-in runtime bindings.
;; References:
;; - std.core
module std.core.primitives(add, sub, mul, div, mod, band, bor, bxor, bshl, bshr, bnot, and, or, eq, lt, le, gt, ge, argv, errno, is_int, is_ptr, is_none, globals, set_globals, argc, envc, envp, __big_add_abs, __big_sub_abs, __big_mul_abs, runtime_tag_raw, init_str_raw, bytes_new_raw, kwarg_new_raw, range_new_raw, list_as_tuple_raw)
comptime template _prim_bin(name, intr, doc){
   @jit
   @inline
   fn name(a, b) {
      doc
      return intr(a, b)
   }
}

comptime template _prim_bin_bool(name, intr, doc){
   @jit
   @inline
   fn name(a, b) bool {
      doc
      return intr(a, b)
   }
}

comptime template _prim_un(name, intr, doc){
   @jit
   @inline
   fn name(x) {
      doc
      return intr(x)
   }
}

comptime template _prim_un_bool(name, intr, doc){
   @jit
   @inline
   fn name(x) bool {
      doc
      return intr(x)
   }
}

comptime template _prim_zero(name, Ret, intr, doc){
   @jit
   @inline
   fn name() Ret {
      doc
      return intr()
   }
}

comptime template _prim_bigbin(name, intr, doc){
   @jit
   @inline
   fn name(a, b) {
      doc
      return intr(a, b)
   }
}

comptime template _prim_untyped_un(name, intr, doc){
   @jit
   @inline
   fn name(x) {
      doc
      return intr(x)
   }
}

comptime template _prim_untyped_bin(name, intr, doc){
   @jit
   @inline
   fn name(a, b) {
      doc
      return intr(a, b)
   }
}

comptime template _prim_untyped_tri(name, intr, doc){
   @jit
   @inline
   fn name(a, b, c) {
      doc
      return intr(a, b, c)
   }
}

comptime emit _prim_bigbin(__big_add_abs, __big_add_abs, "Adds the magnitudes of two bigint values.")
comptime emit _prim_bigbin(__big_sub_abs, __big_sub_abs, "Subtracts bigint magnitudes assuming `a >= b`.")
comptime emit _prim_bigbin(__big_mul_abs, __big_mul_abs, "Multiplies the magnitudes of two bigint values.")
comptime emit _prim_untyped_un(runtime_tag_raw, __runtime_tag, "Returns the runtime tag integer for a named built-in type.")
comptime emit _prim_untyped_bin(init_str_raw, __init_str, "Initializes raw memory as a Nytrix string object.")
comptime emit _prim_untyped_un(bytes_new_raw, __bytes_new, "Allocates a Nytrix bytes object.")
comptime emit _prim_untyped_bin(kwarg_new_raw, __kwarg_new, "Allocates a keyword-argument wrapper object.")
comptime emit _prim_untyped_tri(range_new_raw, __range_new, "Allocates a Nytrix range object.")
comptime emit _prim_untyped_un(list_as_tuple_raw, __list_as_tuple, "Retags a list object as a tuple object.")
comptime emit _prim_bin(add, __add, "Returns the sum of `a` and `b` (untagged).")
comptime emit _prim_bin(sub, __sub, "Returns the difference of `a` and `b` (untagged).")
comptime emit _prim_bin(mul, __mul, "Returns the product of `a` and `b` (untagged).")
comptime emit _prim_bin(div, __div, "Returns the quotient of `a` and `b` (untagged).")
comptime emit _prim_bin(mod, __mod, "Returns the remainder of `a` divided by `b` (untagged).")
comptime emit _prim_bin(band, __and, "Performs bitwise AND on `a` and `b`.")
comptime emit _prim_bin(and, __and, "Performs logical AND on `a` and `b`.")
comptime emit _prim_bin(bor, __or, "Performs bitwise OR on `a` and `b`.")
comptime emit _prim_bin(or, __or, "Performs logical OR on `a` and `b`.")
comptime emit _prim_bin(bxor, __xor, "Performs bitwise XOR on `a` and `b`.")
comptime emit _prim_bin(bshl, __shl, "Performs bitwise shift left on `a` by `b` bits.")
comptime emit _prim_bin(bshr, __shr, "Performs bitwise shift right on `a` by `b` bits.")
comptime emit _prim_un(bnot, __not, "Performs bitwise NOT on `x`.")
comptime emit _prim_bin_bool(eq, __eq, "Returns **true** if `a` and `b` are equal(untagged).")
comptime emit _prim_bin_bool(lt, __lt, "Returns **true** if `a < b` (untagged).")
comptime emit _prim_bin_bool(le, __le, "Returns **true** if `a <= b` (untagged).")
comptime emit _prim_bin_bool(gt, __gt, "Returns **true** if `a > b` (untagged).")
comptime emit _prim_bin_bool(ge, __ge, "Returns **true** if `a >= b` (untagged).")
comptime emit _prim_un_bool(is_int, __is_int, "Returns **true** if `x` is a tagged integer.")

comptime emit _prim_zero(globals, ptr, __globals, "Returns the pointer to the global variable table.")

comptime emit _prim_zero(argc, int, __argc, "Returns the number of command-line arguments.")
comptime emit _prim_zero(envc, int, __envc, "Returns the number of environment variables.")
comptime emit _prim_zero(errno, int, __errno, "Returns the last error number from the system.")

@jit
@inline
fn argv(int i) str {
   "Returns the command-line argument at index `i`."
   return __argv(i)
}

@jit
@inline
fn set_globals(ptr p) ptr {
   "Sets the pointer to the global variable table."
   return __set_globals(p)
}

@jit
@inline
fn envp(int i) ptr {
   "Returns the raw pointer to the environment entry at index `i`."
   return __load64_idx(__envp(), i * 8)
}

@jit
@inline
fn is_ptr(any x) bool {
   "Returns **true** if `x` appears to be a valid heap pointer."
   if(!x){ return false }
   if(__is_int(x)){ return false }
   return(__and(x, 7) == 0)
}

@jit
@inline
fn is_none(any x) bool {
   "Returns **true** if `x` is **none**. Integer 0 is not **none**."
   if(__is_int(x)){ return false }
   return x == nil
}
