;; Keywords: cipher bacon
;; Bacon cipher decoding routines.
;;
;; Reference:
;; - https://www.dcode.fr/bacon-cipher
module std.math.crypto.cipher.bacon(bacon_decode_ab)
use std.core
use std.core.str

fn bacon_decode_ab(str: bits): str {
   "Decode a string like \"AABAA...\" into letters using the 24-letter Bacon table.
   I/J share a codepoint and U/V share a codepoint in this classic variant:
   I and J share 'ABAAA'
   U and V share 'BAABB'."
   def table = [
      ["AAAAA", "A"], ["AAAAB", "B"], ["AAABA", "C"], ["AAABB", "D"],
      ["AABAA", "E"], ["AABAB", "F"], ["AABBA", "G"], ["AABBB", "H"],
      ["ABAAA", "I"], ["ABAAB", "K"], ["ABABA", "L"], ["ABABB", "M"],
      ["ABBAA", "N"], ["ABBAB", "O"], ["ABBBA", "P"], ["ABBBB", "Q"],
      ["BAAAA", "R"], ["BAAAB", "S"], ["BAABA", "T"], ["BAABB", "U"],
      ["BABAA", "W"], ["BABAB", "X"], ["BABBA", "Y"], ["BABBB", "Z"]
   ]
   mut out = Builder((bits.len / 5) + 8)
   mut i = 0
   while(i + 4 < bits.len){
      def chunk = str_slice(bits, i, i + 5, 1)
      mut j = 0
      while(j < table.len){
         def pair = table[j]
         if(pair[0] == chunk){
            out = builder_append(out, to_str(pair[1]))
            break
         }
         j += 1
      }
      i += 5
   }
   def text = builder_to_str(out)
   builder_free(out)
   text
}
