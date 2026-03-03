;; Keywords: core mem
;; Core Mem module.

module std.core.mem (
   memchr, memcpy, memset, memcmp,
   strcpy, cstr, cstr_dup
)
use std.core *

fn memchr(ptr, val, n){
   "Searches for the first occurrence of byte `val` in the first `n` bytes of memory area `ptr`. Returns the address of the byte if found, otherwise 0."
   mut i = 0
   while(i < n){
      def b = load8(ptr, i)
      if(b == val){ return ptr + i  }
      i += 1
   }
   return 0
}

fn memcpy(dst, src, n){
   "Copies `n` bytes from `src` memory address to `dst`. Optimized for 8-byte aligned transfers if both pointers are 8-byte aligned."
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
      def zero = to_int(0)
      while(i + 8 <= n){
         store64(p, zero, i)
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

fn strcpy(dst, src){
   "Copies a NUL-terminated byte string from `src` into `dst` and returns `dst`."
   mut i = 0
   while(true){
      def ch = load8(src, i)
      store8(dst, ch, i)
      if(ch == 0){ return dst }
      i += 1
   }
}

fn cstr(s, fallback=""){
   "Returns a NUL-terminated string suitable for native interop."
   if(!is_str(s)){ s = to_str(s) }
   if(!is_str(s)){ s = fallback }
   if(!is_str(s)){ return "\x00" }
   def n = len(s)
   if(n == 0){ return "\x00" }
   if(load8(s, n - 1) == 0){ return s }
   s + "\x00"
}

fn cstr_dup(s, fallback=""){
   "Allocates and returns a NUL-terminated copy of `s` for native APIs that retain the pointer."
   def src = cstr(s, fallback)
   if(!src){ return 0 }
   def n = len(src)
   if(n == 0){
      def empty = malloc(1)
      if(empty){ store8(empty, 0, 0) }
      return empty
   }
   def has_nul = load8(src, n - 1) == 0
   def bytes = has_nul ? n : (n + 1)
   def dst = malloc(bytes)
   if(!dst){ return 0 }
   memcpy(dst, src, n)
   if(!has_nul){ store8(dst, 0, n) }
   dst
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
   mut ok = true
   mut j = 0
   while(j < len_s){
      if(load8(d, j) != load8(s, j)){ ok = false }
      j += 1
   }
   assert(ok, "memcpy works")
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

   print("Testing optimized paths...")
   def large_sz = 100
   def p1 = malloc(large_sz)
   def p4 = malloc(large_sz)
   memset(p1, 0, large_sz)
   mut k = 0
   while(k < large_sz){
      store8(p4, k, k)
      k += 1
   }
   memcpy(p1, p4, large_sz)
   k = 0
   while(k < large_sz){
      assert(load8(p1, k) == k, f"memcpy optimized at offset {k}")
      k += 1
   }
   memset(p1, 0, large_sz)
   k = 0
   while(k < large_sz){
      assert(load8(p1, k) == 0, f"memset optimized at offset {k}")
      k += 1
   }
   free(p1)
   free(p4)

   print("Testing Comprehensive Optimization Coverage...")

   ; Allocate large enough aligned buffers
   def buf_size = 256
   def src = malloc(buf_size)
   def dst = malloc(buf_size)

   assert((src & 7) == 0, "Source buffer should be 8-byte aligned")
   assert((dst & 7) == 0, "Dest buffer should be 8-byte aligned")

   ; Initialize source with a sequence
   mut i = 0
   while(i < buf_size){
      store8(src, i & 255, i)
      i += 1
   }

   ; 1. Test memcpy with aligned pointers and n > 8 (multiple of 8) -> Optimized Loop
   memset(dst, 0, buf_size)
   memcpy(dst, src, 64)
   ; Check buffer sequence
   i = 0
   while(i < 64){
      def val = load8(dst, i)
      def expected = i & 255
      assert(val == expected, f"memcpy aligned n=64 at index {i}")
      i += 1
   }
   assert(load8(dst, 64) == 0, "memcpy buffer overflow check")

   ; 2. Test memcpy with aligned pointers and n not a multiple of 8 -> Optimized Loop + Cleanup
   memset(dst, 0, buf_size)
   memcpy(dst, src, 67)
   ; Check buffer sequence
   i = 0
   while(i < 67){
      def val = load8(dst, i)
      def expected = i & 255
      assert(val == expected, f"memcpy aligned n=67 at index {i}")
      i += 1
   }
   assert(load8(dst, 67) == 0, "memcpy buffer overflow check")

   ; 3. Test memcpy with unaligned pointers (offset) -> Slow Path
   memset(dst, 0, buf_size)
   ; Use src+1 as source. Expected content: src[1]...src[64]
   memcpy(dst, src + 1, 64)

   i = 0
   while(i < 64){
      def val = load8(dst, i)
      def expected = (i + 1) & 255
      assert(val == expected, f"memcpy unaligned src at {i}")
      i += 1
   }

   memset(dst, 0, buf_size)
   memcpy(dst + 3, src, 64)
   ; Check pre-check (first 3 bytes should be 0)
   i = 0
   while(i < 3){
      def val = load8(dst, i)
      assert(val == 0, f"memcpy unaligned dst pre-check at index {i}")
      i += 1
   }
   ; Check the copied part
   i = 0
   while(i < 64){
      def val = load8(dst + 3, i)
      def expected = i & 255
      assert(val == expected, f"memcpy unaligned dst at {i}")
      i += 1
   }

   ; 4. Test memcpy with n < 8 -> Slow Path
   memset(dst, 0, buf_size)
   memcpy(dst, src, 7)
   ; Check buffer sequence
   i = 0
   while(i < 7){
      def val = load8(dst, i)
      def expected = i & 255
      assert(val == expected, f"memcpy small n=7 at index {i}")
      i += 1
   }
   assert(load8(dst, 7) == 0, "memcpy small n=7 overflow check")

   ; 5. Test memset with val = 0 and aligned pointer -> Optimized Path
   ; Fill dst with garbage first
   i = 0
   while(i < 64){
      store8(dst, 255, i)
      i += 1
   }
   memset(dst, 0, 64)
   ; Check buffer content
   i = 0
   while(i < 64){
      def val = load8(dst, i)
      assert(val == 0, f"memset val=0 aligned n=64 at index {i}")
      i += 1
   }

   ; 6. Test memset with val = 0, aligned, n not multiple of 8 -> Optimized + Cleanup
   i = 0
   while(i < 70){
      store8(dst, 255, i)
      i += 1
   }
   memset(dst, 0, 67)
   ; Check buffer content
   i = 0
   while(i < 67){
      def val = load8(dst, i)
      assert(val == 0, f"memset val=0 aligned n=67 at index {i}")
      i += 1
   }
   assert(load8(dst, 67) == 255, "memset overflow check")

   ; 7. Test memset with val != 0 -> Slow Path
   memset(dst, 0, 64)
   memset(dst, 170, 64) ; 0xAA = 170
   ; Check buffer content
   i = 0
   while(i < 64){
      def val = load8(dst, i)
      assert(val == 170, f"memset val=0xAA at index {i}")
      i += 1
   }

   ; 8. Test memset with unaligned pointer -> Slow Path
   i = 0
   while(i < 64){
      store8(dst, 255, i)
      i += 1
   }
   memset(dst + 1, 0, 60)
   assert(load8(dst) == 255, "memset unaligned pre-check")
   ; check the zeroed part (dst+1 to dst+61)
   i = 1
   while(i < 61){
      assert(load8(dst, i) == 0, f"memset unaligned at {i}")
      i += 1
   }
   assert(load8(dst, 61) == 255, "memset unaligned post-check")

   free(src)
   free(dst)

   print("✓ std.core.mem tests passed")
}
