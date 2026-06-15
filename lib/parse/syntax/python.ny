;; Keywords: syntax python parse highlight
;; Python syntax highlighter.
;; References:
;; - std.parse.syntax
;; - std.parse.syntax.helpers
module std.parse.syntax.python(tokenize)
use std.core
use std.core.str as str
use std.parse.syntax.helpers as _h

def KW = "False;None;True;and;as;assert;async;await;break;class;continue;def;del;elif;else;except;finally;for;from;global;if;import;in;is;lambda;nonlocal;not;or;pass;raise;return;try;while;with;yield"
def FN = "abs;all;any;ascii;bin;bool;breakpoint;bytearray;bytes;callable;chr;classmethod;compile;complex;delattr;dict;dir;divmod;enumerate;eval;exec;filter;float;format;frozenset;getattr;globals;hasattr;hash;hex;id;input;int;isinstance;issubclass;iter;len;list;locals;map;max;min;next;object;oct;open;ord;pow;print;property;range;repr;reversed;round;set;setattr;slice;sorted;staticmethod;str;sum;super;tuple;type;vars;zip"
def CONST = "False;None;True"

fn _prev_non_space(str source, int i) int {
   mut j = i - 1
   while j >= 0 && _h.is_space_ch(load8(source, j)) { j -= 1 }
   j
}

fn _next_non_space(str source, int i, int src_len) int {
   mut j = i
   while j < src_len && _h.is_space_ch(load8(source, j)) { j += 1 }
   j
}

fn tokenize(str source, list out_tokens) list {
   "Runs the tokenize operation."
   def src_len = source.len
   mut i = 0
   mut expect_def_name = false
   mut expect_class_name = false
   while i < src_len {
      def ch = load8(source, i)
      if _h.is_space_ch(ch) {
         def j = _h.scan_space(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 14, i, j - i)
         i = j
      } elif ch == 35 {
         def j = _h.scan_line(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif (ch == 34 || ch == 39) && i + 2 < src_len && load8(source, i + 1) == ch && load8(source, i + 2) == ch {
         def quote = ch
         mut j = i + 3
         while j + 2 < src_len { if load8(source, j) == quote && load8(source, j + 1) == quote && load8(source, j + 2) == quote { j += 3 break } j += 1 }
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif ch == 34 || ch == 39 {
         def j = _h.scan_quoted(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif _h.is_digit_ch(ch) || (ch == 45 && i + 1 < src_len && _h.is_digit_ch(load8(source, i + 1))) {
         def j = _h.scan_number(source, i, src_len, ".xobeE+-J")
         out_tokens = _h.add_tok(out_tokens, 3, i, j - i)
         i = j
      } elif ch == 64 {
         mut j = i + 1
         while j < src_len && _h.is_alnum_ch(load8(source, j)) { j += 1 }
         out_tokens = _h.add_tok(out_tokens, 17, i, j - i)
         i = j
      } elif _h.is_alpha_ch(ch) || ch == 95 {
         def j = _h.scan_ident(source, i, src_len)
         def word = str.str_slice(source, i, j)
         def prev = _prev_non_space(source, i)
         def next = _next_non_space(source, j, src_len)
         def is_call = next < src_len && load8(source, next) == 40
         def is_prop = prev >= 0 && load8(source, prev) == 46
         if _h.in_list(word, CONST) { out_tokens = _h.add_tok(out_tokens, 9, i, j - i) }
         elif expect_def_name {
            out_tokens = _h.add_tok(out_tokens, 5, i, j - i)
            expect_def_name = false
         } elif expect_class_name {
            out_tokens = _h.add_tok(out_tokens, 12, i, j - i)
            expect_class_name = false
         } elif _h.in_list(word, KW) {
            out_tokens = _h.add_tok(out_tokens, 0, i, j - i)
            if word == "def" { expect_def_name = true }
            elif word == "class" { expect_class_name = true }
         }
         elif _h.in_list(word, FN) { out_tokens = _h.add_tok(out_tokens, 5, i, j - i) }
         elif is_call { out_tokens = _h.add_tok(out_tokens, 5, i, j - i) }
         elif is_prop { out_tokens = _h.add_tok(out_tokens, 13, i, j - i) }
         else { out_tokens = _h.add_tok(out_tokens, 8, i, j - i) }
         i = j
      } elif ch == 43 || ch == 45 || ch == 42 || ch == 47 || ch == 37 || ch == 61 || ch == 33 || ch == 60 || ch == 62 || ch == 38 || ch == 124 || ch == 94 || ch == 126 || ch == 63 {
         mut j = i
         while j < src_len { def c = load8(source, j) if c == 43 || c == 45 || c == 42 || c == 47 || c == 37 || c == 61 || c == 33 || c == 60 || c == 62 || c == 38 || c == 124 || c == 94 || c == 126 || c == 63 || c == 62 || c == 47 { j += 1 } else { break } }
         out_tokens = _h.add_tok(out_tokens, 6, i, j - i)
         i = j
      } elif ch == 40 || ch == 41 || ch == 91 || ch == 93 || ch == 123 || ch == 125 || ch == 44 || ch == 58 || ch == 46 {
         out_tokens = _h.add_tok(out_tokens, 7, i, 1)
         i += 1
      } else {
         out_tokens = _h.add_tok(out_tokens, 14, i, 1)
         i += 1
      }
   }
   out_tokens
}
