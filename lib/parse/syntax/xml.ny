;; Keywords: syntax xml parse highlight
;; XML syntax highlighter
;; References:
;; - std.parse.syntax
;; - std.parse.syntax.helpers
module std.parse.syntax.xml(tokenize)
use std.core
use std.core.str as str
use std.parse.syntax.helpers as _h

fn tokenize(str source, list out_tokens) list {
   "Runs the tokenize operation."
   def src_len = source.len
   mut i = 0
   while(i < src_len){
      def idx = i + 0
      i = idx
      def ch = load8(source, i)
      if(_h.is_space_ch(ch)){
         def j = _h.scan_space(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 14, i, j - i)
         i = j
      } elif(ch == 60 && i + 3 < src_len && load8(source, i+1) == 33 && load8(source, i+2) == 45 && load8(source, i+3) == 45){
         mut j = i + 4
         while(j + 2 < src_len){ if(load8(source, j) == 45 && load8(source, j + 1) == 45 && load8(source, j + 2) == 62){ j += 3 break } j += 1 }
         out_tokens = _h.add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif(ch == 60 && i + 1 < src_len && load8(source, i + 1) == 63){
         mut j = i + 2
         while(j + 1 < src_len){ if(load8(source, j) == 63 && load8(source, j + 1) == 62){ j += 2 break } j += 1 }
         out_tokens = _h.add_tok(out_tokens, 10, i, j - i)
         i = j
      } elif(ch == 60){
         mut j = i + 1
         if(j < src_len && load8(source, j) == 47){ j += 1 }
         while(j < src_len && _h.is_alnum_ch(load8(source, j))){ j += 1 }
         out_tokens = _h.add_tok(out_tokens, 18, i, j - i)
         i = j
      } elif(ch == 62){
         out_tokens = _h.add_tok(out_tokens, 7, i, 1)
         i += 1
      } elif(ch == 34 || ch == 39){
         def j = _h.scan_quoted(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif(_h.is_alpha_ch(ch) || ch == 95 || ch == 45 || ch == 46){
         def j = _h.scan_ident_extra(source, i, src_len, "-.")
         if(i > 0 && load8(source, i - 1) == 32){ out_tokens = _h.add_tok(out_tokens, 19, i, j - i) } else { out_tokens = _h.add_tok(out_tokens, 8, i, j - i) }
         i = j
      } elif(ch == 61){
         out_tokens = _h.add_tok(out_tokens, 6, i, 1)
         i += 1
      } elif(ch == 47){
         out_tokens = _h.add_tok(out_tokens, 7, i, 1)
         i += 1
      } else {
         out_tokens = _h.add_tok(out_tokens, 14, i, 1)
         i += 1
      }
   }
   out_tokens
}
