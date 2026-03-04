;; Keywords: core primitives
;; Core Primitives for Nytrix

module std.core.primitives (
   add, sub, mul, div, mod,
   band, bor, bxor, bshl, bshr, bnot,
   and, or,
   eq, lt, le, gt, ge,
   argv, errno,
   is_int, is_ptr, is_none,
   globals, set_globals, argc, envc, envp
)

@inline fn add(a, b){
   "Returns the sum of `a` and `b` (untagged)."
   return __add(a, b)
}

@inline fn sub(a, b){
   "Returns the difference of `a` and `b` (untagged)."
   return __sub(a, b)
}

@inline fn mul(a, b){
   "Returns the product of `a` and `b` (untagged)."
   return __mul(a, b)
}

@inline fn div(a, b){
   "Returns the quotient of `a` and `b` (untagged)."
   return __div(a, b)
}

@inline fn mod(a, b){
   "Returns the remainder of `a` divided by `b` (untagged)."
   return __mod(a, b)
}

@inline fn band(a, b){
   "Performs bitwise AND on `a` and `b`."
   return __and(a, b)
}

@inline fn and(a, b){
   "Performs logical AND on `a` and `b`."
   return __and(a, b)
}

@inline fn bor(a, b){
   "Performs bitwise OR on `a` and `b`."
   return __or(a, b)
}

@inline fn or(a, b){
   "Performs logical OR on `a` and `b`."
   return __or(a, b)
}

@inline fn bxor(a, b){
   "Performs bitwise XOR on `a` and `b`."
   return __xor(a, b)
}

@inline fn bshl(a, b){
   "Performs bitwise shift left on `a` by `b` bits."
   return __shl(a, b)
}

@inline fn bshr(a, b){
   "Performs bitwise shift right on `a` by `b` bits."
   return __shr(a, b)
}

@inline fn bnot(a){
   "Performs bitwise NOT on `a`."
   return __not(a)
}

@inline fn argv(i){
   "Returns the command-line argument at index `i`."
   return __argv(i)
}

@inline fn globals(){
   "Returns the pointer to the global variable table."
   return __globals()
}

@inline fn set_globals(p){
   "Sets the pointer to the global variable table."
   return __set_globals(p)
}

@inline fn argc(){
   "Returns the number of command-line arguments."
   return __argc()
}

@inline fn envc(){
   "Returns the number of environment variables."
   return __envc()
}

@inline fn envp(i){
   "Returns the environment variable string at index `i`."
   return __load64_idx(__envp(), i * 8)
}

@inline fn errno(){
   "Returns the last error number from the system."
   return __errno()
}

@inline fn eq(a, b){
   "Returns **true** if `a` and `b` are equal (untagged)."
   return __eq(a, b)
}

@inline fn lt(a, b){
   "Returns **true** if `a < b` (untagged)."
   return __lt(a, b)
}

@inline fn le(a, b){
   "Returns **true** if `a <= b` (untagged)."
   return __le(a, b)
}

@inline fn gt(a, b){
   "Returns **true** if `a > b` (untagged)."
   return __gt(a, b)
}

@inline fn ge(a, b){
   "Returns **true** if `a >= b` (untagged)."
   return __ge(a, b)
}

fn is_int(x){
   "Returns **true** if `x` is a tagged integer."
   return __is_int(x)
}

fn is_ptr(x){
   "Returns **true** if `x` appears to be a valid heap pointer."
   if(!x){ return false }
   if(__is_int(x)){ return false }
   return (__and(x, 7) == 0)
}

fn is_none(x){
   "Returns **true** if `x` is **none** (0)."
   return x == 0
}
