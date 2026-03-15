;; Keywords: syntax nytrix language
;; Nytrix syntax highlighter
module std.parse.syntax.nytrix(tokenize)
use std.core
use std.core.str as str
use std.parse.syntax.helpers as _h

def KW = "def;mut;del;fn;use;as;if;elif;else;while;for;in;match;case;return;break;continue;assert;print;struct;layout;module;import;comptime;template;emit;table;with;defer;try;catch;true;false;nil;not;and;or;enum;type;impl;operator;extern;static;resource;self;is;from;generated;export;internal"
def FN = "len;append;slice;get;set;put;delete;clear;dict;list;set_idx;items;keys;values;contains;to_str;to_int;is_str;is_int;is_float;is_dict;is_list;is_set;is_tuple;is_bool;type;type_shape;require_shape;env;file_read;file_write;file_exists;chr;ord;range;range2;progress;progress_each;map;filter;reduce;fold;first;last;count;chunk;windowed;max;min;abs;sqrt;sin;cos;tan;log;exp;floor;ceil;round;pow;fmod;ticks;msleep;usleep;spawn;recv;sendline;close;future;async;await;await_all;future_wait;detach"
def TP = "any;bool;int;i8;i16;i32;i64;u8;u16;u32;u64;char;float;f32;f64;f128;number;str;dict;list;set;tuple;bytes;range;seq;ptr;handle;fnptr;void;Result;Option;Error"

fn tokenize(str: source, list: out_tokens): list {
   def src_len = source.len
   mut i = 0
   mut guard = 0
   while(i < src_len && guard < src_len + 8){
      guard += 1
      def idx = i + 0
      i = idx
      def ch = load8(source, i)
      if(_h.is_space_ch(ch)){
         def j = _h.scan_space(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 14, i, j - i)
         i = j
      } elif(ch == 59){
         def j = _h.scan_line(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif((ch == 34 || ch == 39) && i + 2 < src_len && load8(source, i + 1) == ch && load8(source, i + 2) == ch){
         def quote = ch
         mut j = i + 3
         while(j + 2 < src_len){
            if(load8(source, j) == quote && load8(source, j + 1) == quote && load8(source, j + 2) == quote){
               j += 3
               break
            }
            j += 1
         }
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif(ch == 34 || ch == 39){
         def j = _h.scan_quoted(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif(_h.is_digit_ch(ch) || (ch == 45 && i + 1 < src_len && _h.is_digit_ch(load8(source, i + 1)))){
         def j = _h.scan_number(source, i, src_len, ".xobeE+-")
         out_tokens = _h.add_tok(out_tokens, 3, i, j - i)
         i = j
      } elif(_h.is_alpha_ch(ch) || ch == 95){
         def j = _h.scan_ident(source, i, src_len)
         def word = str.str_slice(source, i, j)
         if(_h.in_list(word, KW)){ out_tokens = _h.add_tok(out_tokens, 0, i, j - i) }
         elif(_h.in_list(word, TP)){ out_tokens = _h.add_tok(out_tokens, 1, i, j - i) }
         elif(_h.in_list(word, FN)){ out_tokens = _h.add_tok(out_tokens, 5, i, j - i) }
         else { out_tokens = _h.add_tok(out_tokens, 8, i, j - i) }
         i = j
      } elif(ch == 43 || ch == 45 || ch == 42 || ch == 47 || ch == 37 || ch == 61 || ch == 33 || ch == 60 || ch == 62 || ch == 38 || ch == 124 || ch == 94 || ch == 126 || ch == 63){
         mut j = i
         while(j < src_len){ def c = load8(source, j) if(c == 43 || c == 45 || c == 42 || c == 47 || c == 37 || c == 61 || c == 33 || c == 60 || c == 62 || c == 38 || c == 124 || c == 94 || c == 126 || c == 63 || c == 62){ j += 1 } else { break } }
         out_tokens = _h.add_tok(out_tokens, 6, i, j - i)
         i = j
      } elif(ch == 40 || ch == 41 || ch == 91 || ch == 93 || ch == 123 || ch == 125 || ch == 44 || ch == 59 || ch == 46 || ch == 58 || ch == 64 || ch == 35 || ch == 36){
         out_tokens = _h.add_tok(out_tokens, 7, i, 1)
         i += 1
      } else {
         out_tokens = _h.add_tok(out_tokens, 14, i, 1)
         i += 1
      }
   }
   out_tokens
}
