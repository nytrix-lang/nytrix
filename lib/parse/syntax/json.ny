;; Keywords: syntax json parse highlight
;; JSON syntax highlighter
;; References:
;; - std.parse.syntax
;; - std.parse.syntax.helpers
module std.parse.syntax.json(tokenize)
use std.core
use std.core.str as str
use std.parse.syntax.helpers as _h

fn tokenize(str source, list out_tokens) list {
   "Runs the tokenize operation."
   def src_len = source.len
   mut i = 0
   while i < src_len {
      def ch = load8(source, i)
      if _h.is_space_ch(ch) {
         def j = _h.scan_space(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 14, i, j - i)
         i = j
      } elif ch == 34 {
         def j = _h.scan_quoted(source, i, src_len)
         def next = _h.scan_space(source, j, src_len)
         out_tokens = _h.add_tok(out_tokens, next < src_len && load8(source, next) == 58 ? 20 : 2, i, j - i)
         i = j
      } elif _h.is_digit_ch(ch) || ch == 45 {
         def j = _h.scan_number(source, i, src_len, ".eE+-")
         out_tokens = _h.add_tok(out_tokens, 3, i, j - i)
         i = j
      } elif ch == 116 && i + 3 < src_len && load8(source, i+1) == 114 && load8(source, i+2) == 117 && load8(source, i+3) == 101 {
         out_tokens = _h.add_tok(out_tokens, 9, i, 4)
         i += 4
      } elif ch == 102 && i + 4 < src_len && load8(source, i+1) == 97 && load8(source, i+2) == 108 && load8(source, i+3) == 115 && load8(source, i+4) == 101 {
         out_tokens = _h.add_tok(out_tokens, 9, i, 5)
         i += 5
      } elif ch == 110 && i + 3 < src_len && load8(source, i+1) == 117 && load8(source, i+2) == 108 && load8(source, i+3) == 108 {
         out_tokens = _h.add_tok(out_tokens, 9, i, 4)
         i += 4
      } elif ch == 123 || ch == 125 || ch == 91 || ch == 93 || ch == 44 || ch == 58 {
         out_tokens = _h.add_tok(out_tokens, 7, i, 1)
         i += 1
      } else {
         out_tokens = _h.add_tok(out_tokens, 14, i, 1)
         i += 1
      }
   }
   out_tokens
}
