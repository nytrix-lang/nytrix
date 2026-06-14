;; Keywords: encoding uu math crypto
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc4648
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
;; References:
;; - std.math.crypto.encoding
;; - std.math.crypto
module std.math.crypto.encoding.uu(uu_decode_line)
use std.core
use std.core.str

fn uu_decode_line(any line) str {
   "Decode one uuencoded line to text."
   if !is_str(line) || line.len == 0 { return "" }
   def out_len = (ord_at(line, 0) - 32) & 63
   mut out = ""
   mut emitted = 0
   mut i = 1
   while i + 3 < line.len {
      def a, b = (ord_at(line, i) - 32) & 63, (ord_at(line, i + 1) - 32) & 63
      def c, d = (ord_at(line, i + 2) - 32) & 63, (ord_at(line, i + 3) - 32) & 63
      def b0, b1 = ((a << 2) | (b >> 4)) & 255, (((b & 15) << 4) | (c >> 2)) & 255
      def b2 = (((c & 3) << 6) | d) & 255
      if emitted < out_len { out = str_add(out, chr(b0)) emitted += 1 }
      if emitted < out_len { out = str_add(out, chr(b1)) emitted += 1 }
      if emitted < out_len { out = str_add(out, chr(b2)) emitted += 1 }
      i = i + 4
   }
   return out
}
