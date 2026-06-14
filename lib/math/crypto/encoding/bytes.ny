;; Keywords: encoding bytes math crypto
;; Encoding routines for byte-string conversion and packing.
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc7468
;; References:
;; - std.math.crypto.encoding
;; - std.math.crypto
module std.math.crypto.encoding.bytes(bytes, bytes_set, bytes_get)
use std.core

@returns_owned
fn bytes(int n) bytes {
   "Allocates a bytes buffer of length `n`."
   if n < 0 { n = 0 }
   def p = __bytes_new(n)
   if !p { panic("bytes allocation failed") }
   p
}

fn bytes_set(bytes b, int i, int v) bytes {
   "Stores byte `v` at index `i`."
   if !is_bytes(b) { return b }
   if !is_int(i) { return b }
   def n = b.len
   if i < 0 || i >= n { return b }
   store8(b, v, i)
   b
}

fn bytes_get(bytes b, int i) int {
   "Returns byte at index `i`."
   if !is_bytes(b) { return 0 }
   if !is_int(i) { return 0 }
   def n = b.len
   if i < 0 || i >= n { return 0 }
   load8(b, i)
}
