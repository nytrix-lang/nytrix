;; Keywords: cipher keyboard-shift math crypto keyboard
;; Reference:
;; - https://netlab.cs.ucla.edu/wiki/files/shannon1949.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.keyboard_shift(keyboard_shift_transform, keyboard_shift_decrypt)
use std.core
use std.core.str

fn _row_shift_char(str c, str normal_row, str shifted_row, int shift) str {
   mut i = 0
   while i < utf8_len(normal_row) {
      if str_slice(normal_row, i, i + 1, 1) == c {
         def idx = (i + shift + utf8_len(normal_row)) % utf8_len(normal_row)
         return str_slice(normal_row, idx, idx + 1, 1)
      }
      if str_slice(shifted_row, i, i + 1, 1) == c {
         def idx = (i + shift + utf8_len(shifted_row)) % utf8_len(shifted_row)
         return str_slice(shifted_row, idx, idx + 1, 1)
      }
      i += 1
   }
   c
}

fn keyboard_shift_transform(str text, int shift) str {
   "Shift characters across US QWERTY rows by shift positions."
   def row1 = "`1234567890-="
   def row1s = "~!@#$%^&*()_+"
   def row2 = "qwertyuiop[]\\"
   def row2s = "QWERTYUIOP{}|"
   def row3 = "asdfghjkl;'"
   def row3s = "ASDFGHJKL:\""
   def row4 = "zxcvbnm,./"
   def row4s = "ZXCVBNM<>?"
   mut out = ""
   mut i = 0
   while i < utf8_len(text) {
      def c, a = str_slice(text, i, i + 1, 1), _row_shift_char(c, row1, row1s, shift)
      if a != c {
         out = str_add(out, a)
         i += 1
         continue
      }
      def b = _row_shift_char(c, row2, row2s, shift)
      if b != c {
         out = str_add(out, b)
         i += 1
         continue
      }
      def d = _row_shift_char(c, row3, row3s, shift)
      if d != c {
         out = str_add(out, d)
         i += 1
         continue
      }
      def e = _row_shift_char(c, row4, row4s, shift)
      out = str_add(out, e)
      i += 1
   }
   out
}

fn keyboard_shift_decrypt(str text, int shift=-2) str {
   "Decrypt keyboard-row Caesar text. Default matches the common two-key-row shift variant."
   keyboard_shift_transform(text, shift)
}
