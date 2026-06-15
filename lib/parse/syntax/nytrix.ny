;; Keywords: syntax nytrix language parse highlight
;; Nytrix syntax highlighter
;; References:
;; - std.parse.syntax
;; - std.parse.syntax.helpers
module std.parse.syntax.nytrix(tokenize)
use std.core
use std.core.str as str
use std.parse.syntax.helpers as _h

def KW = "def;mut;del;fn;use;as;if;elif;else;while;for;in;match;case;return;break;continue;assert;print;struct;layout;module;import;comptime;template;emit;table;with;defer;try;catch;true;false;nil;not;and;or;enum;type;impl;operator;extern;static;resource;self;is;from;generated;export;internal"
def FN = "len;append;slice;get;set;put;delete;clear;dict;list;set_idx;items;keys;values;contains;to_str;to_int;is_str;is_int;is_float;is_dict;is_list;is_set;is_tuple;is_bool;type;type_shape;require_shape;env;file_read;file_write;file_exists;chr;ord;range;range2;progress;progress_each;map;filter;reduce;fold;first;last;count;chunk;windowed;max;min;abs;sqrt;sin;cos;tan;log;exp;floor;ceil;round;pow;fmod;ticks;msleep;usleep;spawn;recv;sendline;close;future;async;await;await_all;future_wait;detach"
def TP = "any;bool;int;i8;i16;i32;i64;u8;u16;u32;u64;char;float;f32;f64;f128;number;str;dict;list;set;tuple;bytes;range;seq;ptr;handle;fnptr;void;Result;Option;Error"
def CONST = "true;false;nil;none"
def TYPE_DECL = "struct;enum;type;impl;layout"
def IMPORT_WORDS = "use;import;from"

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

fn _looks_constant(str word) bool {
   def n = word.len
   if n < 2 { return false }
   mut seen_alpha = false
   mut i = 0
   while i < n {
      def c = load8(word, i)
      if c >= 65 && c <= 90 { seen_alpha = true }
      elif (c >= 48 && c <= 57) || c == 95 {}
      else { return false }
      i += 1
   }
   seen_alpha
}

fn _skip_type_suffix(str source, int i, int src_len) int {
   mut j = _next_non_space(source, i, src_len)
   while j < src_len && load8(source, j) == 60 {
      mut depth = 0
      while j < src_len {
         def c = load8(source, j)
         if c == 60 { depth += 1 }
         elif c == 62 {
            depth -= 1
            j += 1
            if depth <= 0 { break }
            continue
         }
         j += 1
      }
      j = _next_non_space(source, j, src_len)
   }
   j
}

fn _param_has_name_after_type(str source, int i, int src_len) bool {
   def j = _skip_type_suffix(source, i, src_len)
   j < src_len && _h.is_alpha_ch(load8(source, j))
}

fn _fn_sig_word_is_type(str source, int start, int stop, str word, int src_len) bool {
   if _h.in_list(word, TP) { return true }
   if _param_has_name_after_type(source, stop, src_len) { return true }
   def prev = _prev_non_space(source, start)
   prev >= 0 && load8(source, prev) == 60
}

fn tokenize(str source, list out_tokens) list {
   "Runs the tokenize operation."
   def src_len = source.len
   mut i = 0
   mut guard = 0
   mut expect_fn_name = false
   mut expect_fn_lparen = false
   mut expect_type_name = false
   mut expect_return_type = false
   mut fn_sig_depth = 0
   mut fn_sig_default = false
   mut import_path = false
   while i < src_len && guard < src_len + 8 {
      guard += 1
      def ch = load8(source, i)
      if _h.is_space_ch(ch) {
         def j = _h.scan_space(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 14, i, j - i)
         i = j
      } elif ch == 59 {
         def j = _h.scan_line(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif (ch == 34 || ch == 39) && i + 2 < src_len && load8(source, i + 1) == ch && load8(source, i + 2) == ch {
         def quote = ch
         mut j = i + 3
         while j + 2 < src_len {
            if load8(source, j) == quote && load8(source, j + 1) == quote && load8(source, j + 2) == quote {
               j += 3
               break
            }
            j += 1
         }
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif ch == 34 || ch == 39 {
         def j = _h.scan_quoted(source, i, src_len)
         out_tokens = _h.add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif (ch == 64 || ch == 35) && i + 1 < src_len && (_h.is_alpha_ch(load8(source, i + 1)) || load8(source, i + 1) == 95) {
         def j = _h.scan_ident_extra(source, i + 1, src_len, "._")
         out_tokens = _h.add_tok(out_tokens, 17, i, j - i)
         i = j
      } elif _h.is_digit_ch(ch) || (ch == 45 && i + 1 < src_len && _h.is_digit_ch(load8(source, i + 1))) {
         def j = _h.scan_number(source, i, src_len, ".xobeE+-_")
         out_tokens = _h.add_tok(out_tokens, 3, i, j - i)
         i = j
      } elif _h.is_alpha_ch(ch) || ch == 95 {
         def j = _h.scan_ident(source, i, src_len)
         def word = str.str_slice(source, i, j)
         def prev = _prev_non_space(source, i)
         def next = _next_non_space(source, j, src_len)
         def is_call = next < src_len && load8(source, next) == 40
         def is_prop = prev >= 0 && load8(source, prev) == 46
         if _h.in_list(word, CONST) { out_tokens = _h.add_tok(out_tokens, 9, i, j - i) }
         elif expect_fn_name {
            out_tokens = _h.add_tok(out_tokens, 5, i, j - i)
            expect_fn_name = false
            expect_fn_lparen = is_call
         } elif expect_type_name {
            out_tokens = _h.add_tok(out_tokens, 12, i, j - i)
            expect_type_name = false
         } elif expect_return_type && (word == "case" || word == "as") {
            out_tokens = _h.add_tok(out_tokens, 0, i, j - i)
            expect_return_type = false
         } elif expect_return_type && !_h.in_list(word, KW) {
            out_tokens = _h.add_tok(out_tokens, 1, i, j - i)
         } elif fn_sig_depth > 0 && !fn_sig_default && !_h.in_list(word, KW) {
            if _fn_sig_word_is_type(source, i, j, word, src_len) { out_tokens = _h.add_tok(out_tokens, 1, i, j - i) }
            else { out_tokens = _h.add_tok(out_tokens, 11, i, j - i) }
         } elif _h.in_list(word, KW) {
            out_tokens = _h.add_tok(out_tokens, 0, i, j - i)
            if word == "fn" {
               expect_fn_name = true
               expect_fn_lparen = false
               expect_return_type = false
            }
            elif _h.in_list(word, TYPE_DECL) { expect_type_name = true }
            elif _h.in_list(word, IMPORT_WORDS) { import_path = true }
            elif word == "as" { import_path = false }
         }
         elif _h.in_list(word, TP) { out_tokens = _h.add_tok(out_tokens, 1, i, j - i) }
         elif _h.in_list(word, FN) { out_tokens = _h.add_tok(out_tokens, 5, i, j - i) }
         elif is_call { out_tokens = _h.add_tok(out_tokens, 5, i, j - i) }
         elif is_prop || import_path { out_tokens = _h.add_tok(out_tokens, 13, i, j - i) }
         elif _looks_constant(word) { out_tokens = _h.add_tok(out_tokens, 9, i, j - i) }
         else { out_tokens = _h.add_tok(out_tokens, 8, i, j - i) }
         i = j
      } elif ch == 43 || ch == 45 || ch == 42 || ch == 47 || ch == 37 || ch == 61 || ch == 33 || ch == 60 || ch == 62 || ch == 38 || ch == 124 || ch == 94 || ch == 126 || ch == 63 {
         mut j = i
         while j < src_len { def c = load8(source, j) if c == 43 || c == 45 || c == 42 || c == 47 || c == 37 || c == 61 || c == 33 || c == 60 || c == 62 || c == 38 || c == 124 || c == 94 || c == 126 || c == 63 || c == 62 { j += 1 } else { break } }
         out_tokens = _h.add_tok(out_tokens, 6, i, j - i)
         if ch == 61 && fn_sig_depth == 1 { fn_sig_default = true }
         i = j
      } elif ch == 40 || ch == 41 || ch == 91 || ch == 93 || ch == 123 || ch == 125 || ch == 44 || ch == 59 || ch == 46 || ch == 58 || ch == 64 || ch == 35 || ch == 36 {
         if ch == 40 && (expect_fn_name || expect_fn_lparen) {
            expect_fn_name = false
            expect_fn_lparen = false
            fn_sig_depth = 1
            fn_sig_default = false
         } elif ch == 40 && fn_sig_depth > 0 {
            fn_sig_depth += 1
         } elif ch == 41 && fn_sig_depth > 0 {
            fn_sig_depth -= 1
            if fn_sig_depth == 0 {
               fn_sig_default = false
               expect_return_type = true
            }
         } elif ch == 44 && fn_sig_depth == 1 {
            fn_sig_default = false
         } elif ch == 123 {
            expect_return_type = false
         }
         out_tokens = _h.add_tok(out_tokens, 7, i, 1)
         i += 1
      } else {
         out_tokens = _h.add_tok(out_tokens, 14, i, 1)
         i += 1
      }
   }
   out_tokens
}
