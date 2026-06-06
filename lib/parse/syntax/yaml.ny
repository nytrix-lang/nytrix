;; Keywords: syntax yaml yml parse highlight
;; YAML syntax highlighter
;; References:
;; - std.parse.syntax
;; - std.parse.syntax.helpers
module std.parse.syntax.yaml(tokenize)
use std.core
use std.core.str as str
use std.parse.syntax.helpers as _h

def KW = "true;false;null;True;False;Null;TRUE;FALSE;NULL;yes;no;Yes;No;YES;NO;on;off;On;Off;ON;OFF"

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
      } elif(ch == 35){
         def j = _h.scan_line(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif(ch == 34 || ch == 39){
         def j = _h.scan_quoted(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif(ch == 45 && i + 1 < src_len && load8(source, i + 1) == 45 && i + 2 < src_len && load8(source, i + 2) == 45){
         out_tokens = _h.add_tok(out_tokens, 6, i, 3)
         i += 3
      } elif(ch == 46 && i + 1 < src_len && load8(source, i + 1) == 46 && i + 2 < src_len && load8(source, i + 2) == 46){
         out_tokens = _h.add_tok(out_tokens, 6, i, 3)
         i += 3
      } elif(_h.is_digit_ch(ch) || (ch == 45 && i + 1 < src_len && _h.is_digit_ch(load8(source, i + 1)))){
         def j = _h.scan_number(source, i, src_len, ".eE+-")
         out_tokens = _h.add_tok(out_tokens, 3, i, j - i)
         i = j
      } elif(_h.is_alpha_ch(ch) || ch == 95){
         def j = _h.scan_ident(source, i, src_len)
         def word = str.str_slice(source, i, j)
         def next = _h.scan_space(source, j, src_len)
         if(next < src_len && load8(source, next) == 58){ out_tokens = _h.add_tok(out_tokens, 20, i, j - i) }
         elif(_h.in_list(word, KW)){ out_tokens = _h.add_tok(out_tokens, 9, i, j - i) }
         else { out_tokens = _h.add_tok(out_tokens, 21, i, j - i) }
         i = j
      } elif(ch == 58){
         out_tokens = _h.add_tok(out_tokens, 7, i, 1)
         i += 1
      } elif(ch == 45){
         mut j = i + 1
         while(j < src_len && load8(source, j) == 32){ j += 1 }
         out_tokens = _h.add_tok(out_tokens, 6, i, j - i)
         i = j
      } else {
         out_tokens = _h.add_tok(out_tokens, 14, i, 1)
         i += 1
      }
   }
   out_tokens
}
