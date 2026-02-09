;; Keywords: core primitives
;; Core Primitives module.

module std.core.primitives (
   add, sub, mul, div, mod,
   band, bor, bxor, bshl, bshr, bnot,
   eq, lt, le, gt, ge,
   argv, errno,
   is_int, is_ptr, is_none,
   globals, set_globals, argc, envc, envp
)

fn add(a, b){
   "Returns `a + b` (numbers, pointer math, or string concatenation)."
   __add(a, b)
}

fn sub(a, b){
   "Returns `a - b`."
   __sub(a, b)
}

fn mul(a, b){
   "Returns `a * b`."
   __mul(a, b)
}

fn div(a, b){
   "Returns `a / b`."
   __div(a, b)
}

fn mod(a, b){
   "Returns `a % b`."
   __mod(a, b)
}

fn band(a, b){
   "Returns bitwise `a & b`."
   __and(a, b)
}

fn bor(a, b){
   "Returns bitwise `a | b`."
   __or(a, b)
}

fn bxor(a, b){
   "Returns bitwise `a ^ b`."
   __xor(a, b)
}

fn bshl(a, b){
   "Returns `a << b`."
   __shl(a, b)
}

fn bshr(a, b){
   "Returns `a >> b`."
   __shr(a, b)
}

fn bnot(a){
   "Returns bitwise `~a`."
   __not(a)
}

fn argv(i){
   "Returns the `i`-th command line argument."
   __argv(i)
}

fn globals() {
   "Returns the pointer to the global variables table."
   __globals()
}

fn set_globals(p) {
   "Sets the pointer to the global variables table."
   __set_globals(p)
}

fn argc() {
   "Returns the number of command-line arguments."
   __argc()
}

fn envc() {
   "Returns the number of environment variables."
   __envc()
}

fn envp() {
   "Returns the raw environment variables pointer."
   __envp()
}

fn errno() {
   "Returns the last error number."
   __errno()
}

fn eq(a, b) {
   "Returns **true** if `a == b` (reference or integer equality)."
   __eq(a, b)
}

fn lt(a, b) {
   "Returns **true** if `a < b` (integer/pointer comparison)."
   __lt(a, b)
}

fn le(a, b) {
   "Returns **true** if `a <= b` (integer/pointer comparison)."
   __le(a, b)
}

fn gt(a, b) {
   "Returns **true** if `a > b` (integer/pointer comparison)."
   __gt(a, b)
}

fn ge(a, b) {
   "Returns **true** if `a >= b` (integer/pointer comparison)."
   __ge(a, b)
}

;; Type Predicates

fn is_int(x) {
   "Returns **true** if `x` is a tagged integer."
   asm("andq $$1, $0; setz %al; movzbq %al, $0; shlq $$1, $0; addq $$2, $0", "=r,0", x)
}

fn is_ptr(x) {
   "Returns **true** if `x` is a pointer (aligned, non-zero)."
   if (__eq(x, 0)) { return false }
   asm("andq $$7, $0; setnz %al; movzbq %al, $0; shlq $$1, $0; addq $$2, $0", "=r,0", x)
}

fn is_none(x) {
   "Returns **true** if `x` is **none** (null)."
   __eq(x, 0)
}
