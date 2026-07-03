;; Keywords: mem memory buffer core
;; Memory operations for raw buffers, typed loads/stores, copying, and allocation boundaries.
;; References:
;; - std.core
module std.core.mem(memchr, memcpy, memset, memcmp, strcpy, cstr, cstr_dup, zalloc)
use std.core

fn memchr(any p, int val, int n) any {
   "Searches for the first occurrence of byte `val` in the first `n` bytes of
   memory area `ptr`. Returns the address of the byte if found, otherwise 0."
   mut i = 0
   while i < n {
      def b = load8(p, i)
      if b == val { return p + i }
      i += 1
   }
   return 0
}

fn memcpy(any dst, any src, int n) any {
   "Copies `n` bytes from `src` memory address to `dst` using an optimized path."
   return __memcpy(dst, src, n)
}

fn memset(any p, int val, int n) any {
   "Fills the first `n` bytes of the memory area pointed to by `p` with the constant byte `val`."
   return __memset(p, val, n)
}

fn memcmp(any p1, any p2, int n) int {
   "Compares the first `n` bytes of memory areas `p1` and `p2` using an optimized path."
   return __memcmp(p1, p2, n)
}

fn strcpy(any dst, any src) any {
   "Copies a NUL-terminated byte string from `src` into `dst` and returns `dst`."
   mut i = 0
   while true {
      def ch = load8(src, i)
      store8(dst, ch, i)
      if ch == 0 { return dst }
      i += 1
   }
}

fn cstr(any s, str fallback="") any {
   "Returns a NUL-terminated string suitable for native interop."
   if !is_str(s) { s = to_str(s) }
   if !is_str(s) { s = fallback }
   if !is_str(s) { return "\x00" }
   def n = s.len
   if n == 0 { return "\x00" }
   if load8(s, n - 1) == 0 { return s }
   s + "\x00"
}

@returns_owned
fn zalloc(int n) any {
   "Allocates `n` bytes of zero-initialized memory. Safe replacement for calloc(1, n)."
   def p = malloc(n)
   if !p { return 0 }
   memset(p, 0, n)
   p
}

@returns_owned
fn cstr_dup(any s, str fallback="") any {
   "Allocates and returns a NUL-terminated copy of `s` for native APIs that retain the pointer."
   def src = cstr(s, fallback)
   def n = src.len
   def dst = malloc(n)
   if !dst { return 0 }
   memcpy(dst, src, n)
   dst
}

#main {
   with ptr dst = malloc(32){
      assert(dst != 0, "mem alloc dst")
      memset(dst, 0, 32)
      memcpy(dst, "abcdef", 6)
      assert(load8(dst, 0) == 97 && load8(dst, 5) == 102, "mem memcpy")
      assert(memchr(dst, 100, 6) == dst + 3, "mem memchr")
      assert(memchr(dst, 122, 6) == 0, "mem memchr missing")
      assert(memcmp(dst, "abcdef", 6) == 0, "mem memcmp equal")
      assert(memcmp(dst, "abcdeg", 6) != 0, "mem memcmp different")
      memset(dst + 1, 65, 4)
      assert(load8(dst, 0) == 97, "mem memset prefix")
      assert(load8(dst, 1) == 65 && load8(dst, 4) == 65, "mem memset body")
      assert(load8(dst, 5) == 102, "mem memset suffix")
   }
   def z = zalloc(8)
   assert(z != 0 && load8(z, 0) == 0 && load8(z, 7) == 0, "mem zalloc")
   free(z)
   def cs = cstr("abc")
   assert(load8(cs, 3) == 0, "mem cstr nul")
   def dup = cstr_dup("xy")
   assert(load8(dup, 0) == 120 && load8(dup, 2) == 0, "mem cstr_dup")
   free(dup)
   print("✓ std.core.mem self-test passed")
}
