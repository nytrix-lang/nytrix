;; Keywords: syntax lua
;; Lua syntax highlighter
module std.parse.syntax.lua(tokenize)
use std.core
use std.core.str as str
use std.parse.syntax.helpers as _h

def KW = "and;break;do;else;elseif;end;false;for;function;goto;if;in;local;nil;not;or;repeat;return;then;true;until;while"
def FN = "print;type;tostring;tonumber;ipairs;pairs;next;select;unpack;require;assert;error;getmetatable;setmetatable;rawget;rawset;rawequal;rawlen;pcall;xpcall;coroutine;string;table;math;os;io;file;debug;package;utf8;load;loadfile;dofile;collectgarbage"

fn tokenize(str: source, list: out_tokens): list {
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
      } elif(ch == 45 && i + 1 < src_len && load8(source, i + 1) == 45){
         mut j = i + 2
         if(j + 1 < src_len && load8(source, j) == 91 && load8(source, j + 1) == 91){ while(j + 1 < src_len){ if(load8(source, j) == 93 && load8(source, j + 1) == 93){ j += 2 break } j += 1 } } else { while(j < src_len && load8(source, j) != 10){ j += 1 } }
         out_tokens = _h.add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif(ch == 34 || ch == 39){
         def j = _h.scan_quoted(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif(_h.is_digit_ch(ch) || (ch == 45 && i + 1 < src_len && _h.is_digit_ch(load8(source, i + 1)))){
         def j = _h.scan_number(source, i, src_len, ".xeE+-")
         out_tokens = _h.add_tok(out_tokens, 3, i, j - i)
         i = j
      } elif(_h.is_alpha_ch(ch) || ch == 95){
         def j = _h.scan_ident(source, i, src_len)
         def word = str.str_slice(source, i, j)
         if(_h.in_list(word, KW)){ out_tokens = _h.add_tok(out_tokens, 0, i, j - i) }
         elif(_h.in_list(word, FN)){ out_tokens = _h.add_tok(out_tokens, 5, i, j - i) }
         else { out_tokens = _h.add_tok(out_tokens, 8, i, j - i) }
         i = j
      } elif(ch == 43 || ch == 45 || ch == 42 || ch == 47 || ch == 37 || ch == 94 || ch == 61 || ch == 33 || ch == 60 || ch == 62 || ch == 38 || ch == 124 || ch == 126 || ch == 35){
         mut j = i
         while(j < src_len){ def c = load8(source, j) if(c == 43 || c == 45 || c == 42 || c == 47 || c == 37 || c == 94 || c == 61 || c == 33 || c == 60 || c == 62 || c == 38 || c == 124 || c == 126 || c == 35 || c == 35){ j += 1 } else { break } }
         out_tokens = _h.add_tok(out_tokens, 6, i, j - i)
         i = j
      } elif(ch == 40 || ch == 41 || ch == 91 || ch == 93 || ch == 123 || ch == 125 || ch == 44 || ch == 59 || ch == 46 || ch == 58 || ch == 35){
         out_tokens = _h.add_tok(out_tokens, 7, i, 1)
         i += 1
      } else {
         out_tokens = _h.add_tok(out_tokens, 14, i, 1)
         i += 1
      }
   }
   out_tokens
}
