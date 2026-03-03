;; Keywords: parse binary endian bytes
;; Small helpers for binary field decoding.

module std.parse.bin (
   u16le, u16be, u32le, u32be, zero_list, from_list
)

use std.core *

fn u16le(s, i){
   "Reads an unsigned 16-bit little-endian value from byte string `s` at offset `i`."
   (load8(s, i) | (load8(s, i + 1) << 8)) & 65535
}

fn u16be(s, i){
   "Reads an unsigned 16-bit big-endian value from byte string `s` at offset `i`."
   ((load8(s, i) << 8) | load8(s, i + 1)) & 65535
}

fn u32le(s, i){
   "Reads an unsigned 32-bit little-endian value from byte string `s` at offset `i`."
   (load8(s, i) | (load8(s, i + 1) << 8) | (load8(s, i + 2) << 16) | (load8(s, i + 3) << 24)) & 4294967295
}

fn u32be(s, i){
   "Reads an unsigned 32-bit big-endian value from byte string `s` at offset `i`."
   (load8(s, i) << 24) | (load8(s, i + 1) << 16) | (load8(s, i + 2) << 8) | load8(s, i + 3)
}

fn zero_list(n){
   "Allocates a list containing `n` zero values."
   mut out = list(n)
   mut i = 0
   while(i < n){
      out = append(out, 0)
      i += 1
   }
   out
}

fn from_list(xs){
   "Converts a list of byte values into a NUL-terminated byte string."
   if(!is_list(xs)){ return 0 }
   def n = len(xs)
   def out = init_str(malloc(n + 1 + 16) + 16, n)
   mut i = 0
   while(i < n){
      store8(out, get(xs, i) & 255, i)
      i += 1
   }
   store8(out, 0, n)
   out
}
