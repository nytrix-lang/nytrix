;; Keywords: core mem
;; Core Mem module.

use std.core
module std.core.mem (
   memchr, memcpy, memset, memcmp
)

fn memchr(ptr, val, n){
   "Find byte in memory."
   def i = 0
   while(i < n){
      if(load8(ptr, i) == val){ return ptr + i  }
      i = i + 1
   }
   return 0
}

fn memcpy(dst, src, n){
   "Copies `n` bytes from `src` memory address to `dst`. Optimized for 8-byte aligned transfers."
   if(n >= 8 && (dst & 7) == 0 && (src & 7) == 0){
      def i = 0
      while(i + 8 <= n){
         store64(dst, load64(src, i), i)
         i = i + 8
      }
      while(i < n){
         store8(dst, load8(src, i), i)
         i = i + 1
      }
      return dst
   }
   i = 0
   while(i < n){
      store8(dst, load8(src, i), i)
      i = i + 1
   }
   return dst
}

fn memset(p, val, n){
   "Fills the first `n` bytes of the memory area pointed to by `p` with the constant byte `val`."
   if(n >= 8 && (p & 7) == 0 && val == 0){
      def i = 0
      while(i + 8 <= n){
         store64(p, 0, i)
         i = i + 8
      }
      while(i < n){
         store8(p, val, i)
         i = i + 1
      }
      return p
   }
   i = 0
   while(i < n){
      store8(p, val, i)
      i = i + 1
   }
   return p
}

fn memcmp(p1, p2, n){
   "Compares the first `n` bytes of memory areas `p1` and `p2`. Returns 0 if equal, or the difference between the first mismatching bytes."
   def i = 0
   while(i < n){
      def b1 = load8(p1, i)
      def b2 = load8(p2, i)
      if(b1 != b2){ return b1 - b2  }
      i = i + 1
   }
   return 0
}