;; Keywords: core primitives
;; Core Primitives module.

module std.core.primitives (
   __add, __sub, __mul, __div, __mod,
   __and, __or, __xor, __shl, __shr, __not,
   __eq, __lt, __le, __gt, __ge,
   __argv, __errno,
   is_int, is_ptr, is_none,
   globals, set_globals, argc, envc, envp
)

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

;; Type Predicates

fn is_int(x) {
   "Returns **true** if `x` is a tagged integer."
   asm("andq $$1, $0; xorq $$1, $0; shlq $$1, $0; addq $$2, $0", "=r,0", x)
}

fn is_ptr(x) {
   "Returns **true** if `x` is a pointer (aligned, non-zero)."
   if (__eq(x, 0)) { return false }
   asm("andq $$7, $0; negq $0; sbbq $0, $0; andq $$2, $0; addq $$2, $0", "=r,0", x)
}

fn is_none(x) {
   "Returns **true** if `x` is **none** (null)."
   __eq(x, 0)
}
