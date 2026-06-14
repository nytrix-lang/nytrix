;; Keywords: cipher bacon math crypto
;; Bacon cipher decoding routines.
;;
;; Reference:
;; - https://www.dcode.fr/bacon-cipher
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.bacon(bacon_decode_ab)
use std.core
use std.core.str

fn bacon_decode_ab(str bits) str {
   "Decode a string like \"AABAA...\" into letters using the 24-letter Bacon table.
   I/J share a codepoint and U/V share a codepoint in this classic variant:
   I and J share 'ABAAA'
   U and V share 'BAABB'."
   mut out = Builder((bits.len / 5) + 8)
   mut i = 0
   while i + 4 < bits.len {
      def chunk = str_slice(bits, i, i + 5, 1)
      def ch = case chunk {
         "AAAAA" -> "A"  "AAAAB" -> "B"  "AAABA" -> "C"  "AAABB" -> "D"
         "AABAA" -> "E"  "AABAB" -> "F"  "AABBA" -> "G"  "AABBB" -> "H"
         "ABAAA" -> "I"  "ABAAB" -> "K"  "ABABA" -> "L"  "ABABB" -> "M"
         "ABBAA" -> "N"  "ABBAB" -> "O"  "ABBBA" -> "P"  "ABBBB" -> "Q"
         "BAAAA" -> "R"  "BAAAB" -> "S"  "BAABA" -> "T"  "BAABB" -> "U"
         "BABAA" -> "W"  "BABAB" -> "X"  "BABBA" -> "Y"  "BABBB" -> "Z"
         _ -> ""
      }
      if ch.len > 0 { out = builder_append(out, ch) }
      i += 5
   }
   def text = builder_to_str(out)
   builder_free(out)
   text
}
