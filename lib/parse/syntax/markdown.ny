;; Keywords: syntax markdown md parse highlight
;; Markdown syntax highlighter
;; References:
;; - std.parse.syntax
;; - std.parse.syntax.helpers
module std.parse.syntax.markdown(tokenize)
use std.core
use std.core.str as str
use std.parse.syntax.helpers as _h

fn tokenize(str source, list out_tokens) list {
   "Runs the tokenize operation."
   def src_len = source.len
   mut i = 0
   while i < src_len {
      def idx = i + 0
      i = idx
      def ch = load8(source, i)
      if ch == 10 || ch == 13 {
         mut j = i
         while j < src_len { def c = load8(source, j) if c != 10 && c != 13 { break } j += 1 }
         out_tokens = _h.add_tok(out_tokens, 14, i, j - i)
         i = j
      } elif ch == 35 && (i == 0 || load8(source, i - 1) == 10) {
         mut j = i
         while j < src_len && load8(source, j) == 35 { j += 1 }
         out_tokens = _h.add_tok(out_tokens, 0, i, j - i)
         i = j
      } elif ch == 42 || ch == 95 {
         def c = ch
         mut j = i + 1
         if j < src_len && load8(source, j) == c && j + 1 < src_len && load8(source, j + 1) == c {
            while j + 2 < src_len { if load8(source, j) == c && load8(source, j + 1) == c && load8(source, j + 2) == c { j += 3 break } j += 1 }
            out_tokens = _h.add_tok(out_tokens, 5, i, j - i)
            i = j
         } elif j < src_len && load8(source, j) == c {
            while j + 1 < src_len { if load8(source, j) == c && load8(source, j + 1) == c { j += 2 break } j += 1 }
            out_tokens = _h.add_tok(out_tokens, 1, i, j - i)
            i = j
         } else {
            while j < src_len && load8(source, j) != c && load8(source, j) != 10 && load8(source, j) != 32 { j += 1 }
            if j < src_len && load8(source, j) == c { j += 1 }
            out_tokens = _h.add_tok(out_tokens, 1, i, j - i)
            i = j
         }
      } elif ch == 96 {
         mut j = i + 1
         while j < src_len && load8(source, j) != 96 && load8(source, j) != 10 { j += 1 }
         if j < src_len && load8(source, j) == 96 { j += 1 }
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif ch == 33 && i + 1 < src_len && load8(source, i + 1) == 91 {
         mut j = i + 2
         while j < src_len && load8(source, j) != 93 { j += 1 }
         if j < src_len { j += 1 }
         if j < src_len && load8(source, j) == 40 {
            while j < src_len && load8(source, j) != 41 { j += 1 }
            if j < src_len { j += 1 }
         }
         out_tokens = _h.add_tok(out_tokens, 5, i, j - i)
         i = j
      } elif ch == 91 {
         mut j = i + 1
         while j < src_len && load8(source, j) != 93 { j += 1 }
         if j < src_len { j += 1 }
         if j < src_len && load8(source, j) == 40 {
            while j < src_len && load8(source, j) != 41 { j += 1 }
            if j < src_len { j += 1 }
         }
         out_tokens = _h.add_tok(out_tokens, 5, i, j - i)
         i = j
      } elif ch == 60 {
         mut j = i + 1
         while j < src_len && load8(source, j) != 62 { j += 1 }
         if j < src_len { j += 1 }
         out_tokens = _h.add_tok(out_tokens, 5, i, j - i)
         i = j
      } elif ch == 32 || ch == 9 {
         mut j = i
         while j < src_len { def c = load8(source, j) if c != 32 && c != 9 { break } j += 1 }
         out_tokens = _h.add_tok(out_tokens, 14, i, j - i)
         i = j
      } else {
         out_tokens = _h.add_tok(out_tokens, 14, i, 1)
         i += 1
      }
   }
   out_tokens
}
