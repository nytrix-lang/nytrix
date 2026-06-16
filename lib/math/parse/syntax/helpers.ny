;; Keywords: syntax helpers parse highlight
;; Shared scanning primitives for syntax tokenizers and highlighters.
;; References:
;; - std.math.parse.syntax
module std.math.parse.syntax.helpers(is_alpha_ch, is_digit_ch, is_alnum_ch, is_space_ch, word_eq, lower_ch, in_list, in_list_ci, add_tok, scan_space, scan_line, scan_quoted, scan_ident, char_in, scan_ident_extra, scan_number, tokenize_c_like)
use std.core
use std.core.str as str

fn is_alpha_ch(int b) bool {
   "Returns true for ASCII letters and underscore."
   (b >= 65 && b <= 90) || (b >= 97 && b <= 122) || b == 95
}

fn is_digit_ch(int b) bool {
   "Returns true for ASCII decimal digits."
   b >= 48 && b <= 57
}

fn is_alnum_ch(int b) bool {
   "Returns true for ASCII identifier characters."
   is_alpha_ch(b) || (b >= 48 && b <= 57)
}

fn is_space_ch(int b) bool {
   "Returns true for spaces, tabs, carriage returns, and newlines."
   b == 32 || b == 9 || b == 10 || b == 13
}

fn word_eq(str text, str word) bool {
   "Compares two syntax words byte-for-byte."
   def n = text.len
   if n != word.len { return false }
   mut i = 0
   while i < n {
      if load8(text, i) != load8(word, i) { return false }
      i += 1
   }
   true
}

fn _segment_eq(str text, str list_str, int start, int stop) bool {
   "Compares `text` with a segment in a semicolon-separated word list."
   def n = text.len
   if stop - start != n { return false }
   mut i = 0
   while i < n {
      if load8(text, i) != load8(list_str, start + i) { return false }
      i += 1
   }
   true
}

fn lower_ch(int b) int {
   "Lowercases one ASCII byte without changing non-uppercase bytes."
   if b >= 65 && b <= 90 { return b + 32 }
   b
}

fn _segment_eq_ci(str text, str list_str, int start, int stop) bool {
   "Case-insensitively compares `text` with a segment in a word list."
   def n = text.len
   if stop - start != n { return false }
   mut i = 0
   while i < n {
      if lower_ch(load8(text, i)) != load8(list_str, start + i) { return false }
      i += 1
   }
   true
}

fn in_list(str text, str list_str) bool {
   "Returns true when `text` appears in a semicolon-separated word list."
   def list_n = list_str.len
   mut start = 0
   mut i = 0
   while i <= list_n {
      if i == list_n || load8(list_str, i) == 59 {
         if _segment_eq(text, list_str, start, i) { return true }
         start = i + 1
      }
      i += 1
   }
   false
}

fn in_list_ci(str text, str list_str) bool {
   "Case-insensitively checks a semicolon-separated word list."
   def list_n = list_str.len
   mut start = 0
   mut i = 0
   while i <= list_n {
      if i == list_n || load8(list_str, i) == 59 {
         if _segment_eq_ci(text, list_str, start, i) { return true }
         start = i + 1
      }
      i += 1
   }
   false
}

fn add_tok(list out_tokens, int kind, int start, int tok_len) list {
   "Appends a `[kind, start, length]` syntax token."
   out_tokens = out_tokens.append([kind, start, tok_len])
   out_tokens
}

fn scan_space(str source, int i, int src_len) int {
   "Scans contiguous whitespace from byte index `i`."
   mut j = i
   while j < src_len && is_space_ch(load8(source, j)) { j += 1 }
   return j
}

fn scan_line(str source, int i, int src_len) int {
   "Scans to the end of the current line without consuming the newline."
   mut j = i
   while j < src_len && load8(source, j) != 10 { j += 1 }
   return j
}

fn scan_quoted(str source, int i, int src_len) int {
   "Scans a single- or double-quoted string with backslash escapes."
   def quote = load8(source, i)
   mut j = i + 1
   while j < src_len {
      def c = load8(source, j)
      if c == 92 { j += 2 continue }
      if c == quote { j += 1 break }
      j += 1
   }
   return j
}

fn scan_ident(str source, int i, int src_len) int {
   "Scans an ASCII identifier from byte index `i`."
   mut j = i
   while j < src_len && is_alnum_ch(load8(source, j)) { j += 1 }
   return j
}

fn char_in(str chars, int c) bool {
   "Returns true when byte `c` appears in `chars`."
   mut i = 0
   while i < chars.len {
      if load8(chars, i) == c { return true }
      i += 1
   }
   false
}

fn scan_ident_extra(str source, int i, int src_len, str extra_chars) int {
   "Scans an identifier that may also contain bytes from `extra_chars`."
   mut j = i
   while j < src_len {
      def c = load8(source, j)
      if is_alnum_ch(c) || char_in(extra_chars, c) { j += 1 }
      else { break }
   }
   return j
}

fn scan_number(str source, int i, int src_len, str extra_chars) int {
   "Scans a numeric token, allowing optional leading minus and extra bytes."
   mut j = i
   if j < src_len && load8(source, j) == 45 { j += 1 }
   while j < src_len {
      def c = load8(source, j)
      if is_digit_ch(c) || char_in(extra_chars, c) { j += 1 }
      else { break }
   }
   return j
}

fn _scan_block_comment(str source, int i, int src_len) int {
   mut j = i + 2
   while j + 1 < src_len {
      if load8(source, j) == 42 && load8(source, j + 1) == 47 { return j + 2 }
      j += 1
   }
   j
}

fn _scan_run_chars(str source, int i, int src_len, str chars) int {
   mut j = i
   while j < src_len && char_in(chars, load8(source, j)) { j += 1 }
   j
}

fn _prev_non_space(str source, int i) int {
   mut j = i - 1
   while j >= 0 && is_space_ch(load8(source, j)) { j -= 1 }
   j
}

fn _next_non_space(str source, int i, int src_len) int {
   mut j = i
   while j < src_len && is_space_ch(load8(source, j)) { j += 1 }
   j
}

fn tokenize_c_like(
   str source, list out_tokens, str kw, str types="", str funcs="",
   str ident_extra="", str number_extra=".xobeE+-", str op_chars="+-*/%=!<>&|^~?",
   str punct_chars="()[]{};,.:", int line_comment=47, bool block_comment=true,
   int hash_kind=-1, bool dot_number=true, bool minus_number=false,
) list {
   "Tokenizes C-like syntaxes from shared keyword/type/function tables."
   def src_len = source.len
   mut i = 0
   while i < src_len {
      def ch = load8(source, i)
      if is_space_ch(ch) {
         def j = scan_space(source, i, src_len)
         out_tokens = add_tok(out_tokens, 14, i, j - i)
         i = j
      } elif hash_kind >= 0 && ch == 35 {
         def j = scan_line(source, i, src_len)
         out_tokens = add_tok(out_tokens, hash_kind, i, j - i)
         i = j
      } elif line_comment >= 0 && ch == line_comment && i + 1 < src_len && load8(source, i + 1) == line_comment {
         def j = scan_line(source, i, src_len)
         out_tokens = add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif block_comment && ch == 47 && i + 1 < src_len && load8(source, i + 1) == 42 {
         def j = _scan_block_comment(source, i, src_len)
         out_tokens = add_tok(out_tokens, 4, i, j - i)
         i = j
      } elif ch == 34 || ch == 39 {
         def j = scan_quoted(source, i, src_len)
         out_tokens = add_tok(out_tokens, 2, i, j - i)
         i = j
      } elif ch == 64 && i + 1 < src_len && (is_alpha_ch(load8(source, i + 1)) || load8(source, i + 1) == 95) {
         def j = scan_ident_extra(source, i + 1, src_len, ident_extra)
         out_tokens = add_tok(out_tokens, 17, i, j - i)
         i = j
      } elif is_digit_ch(ch) || (dot_number && ch == 46 && i + 1 < src_len && is_digit_ch(load8(source, i + 1))) || (minus_number && ch == 45 && i + 1 < src_len && is_digit_ch(load8(source, i + 1))) {
         def j = scan_number(source, i, src_len, number_extra)
         out_tokens = add_tok(out_tokens, 3, i, j - i)
         i = j
      } elif is_alpha_ch(ch) || char_in(ident_extra, ch) {
         def j = scan_ident_extra(source, i, src_len, ident_extra)
         def word = str.str_slice(source, i, j)
         def prev = _prev_non_space(source, i)
         def next = _next_non_space(source, j, src_len)
         def is_call = next < src_len && load8(source, next) == 40
         def is_prop = prev >= 0 && load8(source, prev) == 46
         if in_list(word, kw) { out_tokens = add_tok(out_tokens, 0, i, j - i) }
         elif types.len > 0 && in_list(word, types) { out_tokens = add_tok(out_tokens, 1, i, j - i) }
         elif funcs.len > 0 && in_list(word, funcs) { out_tokens = add_tok(out_tokens, 5, i, j - i) }
         elif is_call { out_tokens = add_tok(out_tokens, 5, i, j - i) }
         elif is_prop { out_tokens = add_tok(out_tokens, 13, i, j - i) }
         else { out_tokens = add_tok(out_tokens, 8, i, j - i) }
         i = j
      } elif char_in(op_chars, ch) {
         def j = _scan_run_chars(source, i, src_len, op_chars)
         out_tokens = add_tok(out_tokens, 6, i, j - i)
         i = j
      } elif char_in(punct_chars, ch) {
         out_tokens = add_tok(out_tokens, 7, i, 1)
         i += 1
      } else {
         out_tokens = add_tok(out_tokens, 14, i, 1)
         i += 1
      }
   }
   out_tokens
}
