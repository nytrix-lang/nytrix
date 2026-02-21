;; Keywords: core mem
;; Core Mem module.

module std.core.mem (
   memchr, memcpy, memset, memcmp
)
use std.core *

fn memchr(ptr, val, n){
   "Find byte in memory."
   mut i = 0
   while(i < n){
      if(load8(ptr, i) == val){ return ptr + i  }
      i += 1
   }
   return 0
}

fn memcpy(dst, src, n){
   "Copies `n` bytes from `src` memory address to `dst`. Optimized for 8-byte aligned transfers."
   mut i = 0
   if(n >= 8 && (dst & 7) == 0 && (src & 7) == 0){
      while(i + 8 <= n){
         store64(dst, load64(src, i), i)
         i = i + 8
      }
      while(i < n){
         store8(dst, load8(src, i), i)
         i += 1
      }
      return dst
   }
   i = 0
   while(i < n){
      store8(dst, load8(src, i), i)
      i += 1
   }
   return dst
}

fn memset(p, val, n){
   "Fills the first `n` bytes of the memory area pointed to by `p` with the constant byte `val`."
   mut i = 0
   if(n >= 8 && (p & 7) == 0 && val == 0){
      while(i + 8 <= n){
         store64(p, 0, i)
         i = i + 8
      }
      while(i < n){
         store8(p, val, i)
         i += 1
      }
      return p
   }
   i = 0
   while(i < n){
      store8(p, val, i)
      i += 1
   }
   return p
}

fn memcmp(p1, p2, n){
   "Compares the first `n` bytes of memory areas `p1` and `p2`. Returns 0 if equal, or the difference between the first mismatching bytes."
   mut i = 0
   while(i < n){
      def b1 = load8(p1, i)
      def b2 = load8(p2, i)
      if(b1 != b2){ return b1 - b2  }
      i += 1
   }
   return 0
}

if(comptime{__main()}){
    use std.core
    use std.core.mem *
    use std.str *
    use std.str.io *

    print("Testing memcpy...")
    def s = "hello world"
    def len_s = str_len(s)
    def d = malloc(len_s + 1)
    store64(d, 241, -8)
    store64(d, len_s, -16)
    memcpy(d, s, len_s)
    store8(d, 0, len_s)
    assert(_str_eq(d, s), "memcpy works")
    free(d)

    print("Testing memset...")
    def p = malloc(16)
    memset(p, 0, 16)
    assert(load64(p) == 0, "memset works 0-8")
    assert(load64(p, 8) == 0, "memset works 8-16")
    memset(p, 65, 4) ; 'A'
    assert(load8(p) == 65, "memset 8-bit")
    assert(load8(p, 3) == 65, "memset 8-bit end")
    free(p)

    print("Testing memchr...")
    def s2 = "abcdef"
    mut p2 = memchr(s2, 100, 6) ; 'd'
    assert(p2 == s2 + 3, "memchr found char")
    mut p3 = memchr(s2, 122, 6) ; 'z'
    assert(p3 == 0, "memchr not found")

    print("Testing memcmp...")
    assert(memcmp("abc", "abc", 3) == 0, "memcmp equal")
    assert(memcmp("abc", "abd", 3) != 0, "memcmp unequal")

    print("✓ std.core.mem tests passed")
}
