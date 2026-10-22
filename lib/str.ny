;; Keywords: str text string utf8 ascii core
;; String operations for slicing, splitting, case conversion, parsing, and byte/text boundaries.
;; References:
;; - core
module core.str(len, find, find_from, find_last, _str_eq, cstr_to_str, pad_start, startswith, endswith, atoi, parse_int, atof, split, strip, str_add, upper, lower, str_contains, join, join_words, str_replace, to_hex, to_fixed, chr, repeat, ord, hex_val, utf8_valid, utf8_len, ord_at, str_slice, utf8_slice, split_words, word_frequency, byte_at, ascii_is_lower, ascii_is_upper, ascii_is_alpha, ascii_is_digit, ascii_is_alnum, ascii_is_space, ascii_is_hex_digit, ascii_is_oct_digit, ascii_is_punctuation, ascii_is_printable, ascii_lower_byte, ascii_upper_byte, ascii_only, ascii_all_alpha, ascii_all_digit, ascii_all_alnum, ascii_all_space, ascii_all_printable, _utf8_seq_len, _utf8_decode_at, _utf8_encode_at, _substr, Builder, builder_append, builder_append_byte, builder_to_str, builder_free)
use core
use core as core
use core.primitives (envc, envp)

fn _env_present(str key) bool {
   ;; __env_get returns a managed string for `NAME=...` entries and 0 when
   ;; absent, replacing the manual envp scan (the old scan also misread
   ;; prefix-colliding names like `NY_HOME` for `NY_H`).
   __env_get(key) != 0
}

mut _text_debug_flag = -1

fn _text_debug_enabled() bool {
   if _text_debug_flag == -1 { _text_debug_flag = _env_present("NY_TEXT_DEBUG") ? 1 : 0 }
   _text_debug_flag == 1
}

@inline
fn _match_at(str s, str sub, int at) bool {
   def bound = sub.len
   mut j = 0
   while j < bound {
      if load8(s, at + j) != load8(sub, j) { return false }
      j += 1
   }
   true
}

@inline
fn _ascii_eq_fold(int a, int b) bool {
   if a >= 65 && a <= 90 { a += 32 }
   if b >= 65 && b <= 90 { b += 32 }
   a == b
}

@inline
fn _match_at_ascii_ci(str s, str sub, int at) bool {
   if at < 0 || at + sub.len > s.len { return false }
   mut j = 0
   while j < sub.len {
      if !_ascii_eq_fold(load8(s, at + j), load8(sub, j)) { return false }
      j += 1
   }
   true
}

fn len(any s) int {
   "Returns the number of bytes for strings, otherwise forwards to the value length."
   if __is_str_obj(s) { return __str_len(s) }
   return s.len
}

fn find(str s, str sub) int {
   "Returns the index of the first occurrence of `sub` in `s`, or -1."
   mut int n, m = s.len, sub.len
   if m == 0 { return 0 }
   if n < m { return -1 }
   mut int i = 0
   while i + m <= n {
      if _match_at(s, sub, i) { return i }
      i += 1
   }
   return -1
}

fn find_from(str s, str sub, int start) int {
   "Returns the index of the first occurrence of `sub` in `s` at/after `start`, or -1."
   mut int n, m = s.len, sub.len
   if m == 0 {
      if start < 0 { return 0 }
      if start > n { return n }
      return start
   }
   if start < 0 { start = 0 }
   if n < m || start + m > n { return -1 }
   mut int i = start
   while i + m <= n {
      if _match_at(s, sub, i) { return i }
      i += 1
   }
   -1
}

fn find_last(str s, str sub) int {
   "Returns the index of the last occurrence of `sub` in `s`, or -1."
   mut int n, m = s.len, sub.len
   if m == 0 { return n }
   if n < m { return -1 }
   mut int i = n - m
   while i >= 0 {
      if _match_at(s, sub, i) { return i }
      i -= 1
   }
   return -1
}

fn _str_eq(any a, any b) bool {
   if !is_str(a) || !is_str(b) { return false }
   return __str_eq(a, b)
}

@returns_owned
fn cstr_to_str(any p, int offset=0) str {
   "Converts a C-string pointer to a Nytrix string. Optional offset skips bytes."
   if !p { return "" }
   ;; Raw FFI pointers are not classified by `is_str`; only managed Nytrix
   ;; strings are. Convert a C string explicitly here before it crosses an
   ;; `any` return or enters ordinary string operations.
   ;; Check ownership before querying the string predicate: native FFI
   ;; pointers can use the encoded-pointer representation, and `is_str` may
   ;; inspect string bytes before it can reject an unmanaged pointer.
   if __is_ny_obj(p) && is_str(p) {
      ;; A managed string is already canonical. Keep zero-offset conversion
      ;; idempotent; routing it through `_substr` reinterprets its metadata.
      if offset == 0 { return p }
      return _substr(p, offset, p.len)
   }
   ;; Preserve raw C pointers at the native ABI boundary.
   if offset == 0 { return __cstr_to_str(p) }
   def res = __cstr_to_str(p + offset)
   if !res { return "" }
   res
}

@returns_owned
fn pad_start(str s, int width, str pad=" ") str {
   "Left-pads string `s` to `width` using `pad` (default space)."
   mut int n = s.len
   if n >= width { return _substr(s, 0, n) }
   mut int pad_len = pad.len
   if pad_len == 0 { return _substr(s, 0, n) }
   def int total = width
   mut out = malloc(total + 1)
   if !out { return "" }
   init_str(out, total)
   def int pad_needed = width - n
   mut int i = 0
   while i < pad_needed {
      store8(out, load8(pad, i % pad_len), i)
      i += 1
   }
   mut int j = 0
   while j < n {
      store8(out, load8(s, j), pad_needed + j)
      j += 1
   }
   store8(out, 0, total)
   init_str(out, total)
}

@inline
fn startswith(any s, any prefix) bool {
   "Returns true if string `s` starts with `prefix`."
   if !is_str(s) || !is_str(prefix) { return false }
   mut n = __str_len(prefix)
   def sl = __str_len(s)
   if sl < n { return false }
   return _match_at(s, prefix, 0)
}

fn atoi(any s) int {
   "Parses a decimal integer from string `s`."
   parse_int(s, 10)
}

@inline
fn _ascii_radix_value(int c) int {
   case c {
      48..57 -> c - 48
      65..90 -> c - 55
      97..122 -> c - 87
      _ -> -1
   }
}

@inline
fn _ascii_decimal_value(int c) int {
   case c {
      48..57 -> c - 48
      _ -> -1
   }
}

fn parse_int(any s, int base=10) int {
   "Parses an integer from string `s` using base 2..36."
   if !is_str(s) { return 0 }
   if base < 2 || base > 36 { return 0 }
   def int n = __str_len(s)
   if n == 0 { return 0 }
   mut int sign = 1
   mut int i = 0
   if load8(s, 0) == 45 {
      sign = -1
      i = 1
   }
   mut int out = 0
   while i < n {
      def int digit_byte = load8(s, i)
      def int v = _ascii_radix_value(digit_byte)
      if v < 0 || v >= base { break }
      out = out * base + v
      i += 1
   }
   out * sign
}

fn hex_val(int c) int {
   "Returns the integer value of a hexadecimal character byte `c`."
   def v = _ascii_radix_value(c)
   (v >= 0 && v < 16) ? v : 0
}

fn _atof_unsigned_from(str s, int i0) f64 {
   mut n, i = s.len, i0
   mut val = 0.0
   while i < n {
      def d = _ascii_decimal_value(load8(s, i))
      if d < 0 { break }
      val = val * 10.0 + d
      i += 1
   }
   if i < n && load8(s, i) == 46 {
      i += 1
      mut frac = 0.1
      while i < n {
         def d = _ascii_decimal_value(load8(s, i))
         if d < 0 { break }
         val = val + d * frac
         frac = frac * 0.1
         i += 1
      }
   }
   if i < n && (load8(s, i) == 101 || load8(s, i) == 69) {
      i += 1
      mut esign = 1
      if i < n && load8(s, i) == 45 { esign = -1 i += 1 }
      elif i < n && load8(s, i) == 43 { i += 1 }
      mut eval = 0
      while i < n {
         def d = _ascii_decimal_value(load8(s, i))
         if d < 0 { break }
         eval = eval * 10 + d
         i += 1
      }
      mut e = 0
      mut epow = 1.0
      while e < eval {
         epow = epow * 10.0
         e += 1
      }
      if esign > 0 { val = val * epow }
      else { val = val / epow }
   }
   val
}

fn atof(any s) f64 {
   "Parses a float from string `s`."
   if !is_str(s) { return 0.0 }
   def n = s.len
   if n == 0 { return 0.0 }
   mut sign = 1
   mut i = 0
   def first = load8(s, 0)
   if first == 45 { sign = -1 i = 1 }
   elif first == 43 { i = 1 }
   if i >= n { return 0.0 }
   if i + 3 == n && _match_at_ascii_ci(s, "nan", i) { return __flt_nan() }
   if i + 3 == n && _match_at_ascii_ci(s, "inf", i) {
      if sign < 0 { return -__flt_inf() }
      return __flt_inf()
   }
   if i + 8 == n && _match_at_ascii_ci(s, "infinity", i) {
      if sign < 0 { return -__flt_inf() }
      return __flt_inf()
   }
   def val = _atof_unsigned_from(s, i)
   sign < 0 ? -val : val
}

fn _list_push_reserved(list lst, any v) list {
   ; Native append may return a replacement handle (capacity growth).  The
   ; caller must rebind it: a callee-local reassignment is invisible to the
   ; caller's binding, which silently lost every split/utf8 push.
   return __append(lst, v)
}

@returns_owned
fn _substr(str s, int start, int stop) str {
   mut int n = s.len
   if start < 0 { start = 0 }
   if stop > n { stop = n }
   if start >= stop { return "" }
   def len = stop - start
   mut out = __malloc(len + 1)
   if !out { return "" }
   init_str(out, len)
   mut i = 0
   while i < len {
      store8(out, load8(s, start + i), i)
      i += 1
   }
   store8(out, 0, len)
   return out
}

fn _is_ws(int c) bool { c == 32 || c == 9 || c == 10 || c == 11 || c == 12 || c == 13 }

fn strip(any s) str {
   "Returns `s` without leading/trailing ASCII whitespace."
   if !is_str(s) { return "" }
   mut n = s.len
   if n == 0 { return "" }
   mut start = 0
   while start < n && _is_ws(load8(s, start)) { start += 1 }
   mut end = n
   while end > start && _is_ws(load8(s, end - 1)) { end -= 1 }
   return _substr(s, start, end)
}

@returns_owned
fn _split_impl(str s, str sep) list {
   "Splits string `s` by separator `sep` and returns a list of strings."
   if _text_debug_enabled() { print("Text: split s='" + s + "' sep='" + sep + "'") }
   mut int sep_len = sep.len
   if sep_len == 0 {
      def int chars = utf8_len(s)
      mut out = list(chars)
      mut ci = 0
      while ci < chars {
         out = _list_push_reserved(out, chr(ord_at(s, ci)))
         ci += 1
      }
      return out
   }
   mut int n = s.len
   mut int parts = 1
   mut int scan = 0
   while scan <= n - sep_len {
      if _match_at(s, sep, scan) {
         parts += 1
         scan = scan + sep_len
      } else {
         scan += 1
      }
   }
   mut out = list(parts)
   mut int i = 0
   mut int start = 0
   while i <= n - sep_len {
      def m = _match_at(s, sep, i)
      if m {
         out = _list_push_reserved(out, _substr(s, start, i))
         i = i + sep_len
         start = i
      } else {
         i += 1
      }
   }
   out = _list_push_reserved(out, _substr(s, start, n))
   if _text_debug_enabled() { print("Text: split returning count=" + to_str(out.len)) }
   return out
}

@returns_owned
fn split(any s, any sep) list {
   return _split_impl(s, sep)
}

@returns_owned
fn split_words(any s) list {
   "Splits string `s` into words, automatically trimming and ignoring empty segments."
   if !is_str(s) { return list(0) }
   def int n = s.len
   mut int start = 0
   while start < n && _is_ws(load8(s, start)) { start += 1 }
   mut int end = n
   while end > start && _is_ws(load8(s, end - 1)) { end -= 1 }
   ;; Count nonempty tokens first so the result has its final capacity. Keep
   ;; the historical rule: literal spaces separate words; other whitespace is
   ;; trimmed only at the edges of each word.
   mut int count = 0
   mut int scan = start
   while scan < end {
      while scan < end && load8(s, scan) == 32 { scan += 1 }
      if scan < end {
         def int token_start = scan
         while scan < end && load8(s, scan) != 32 { scan += 1 }
         mut int token_end = scan
         while token_start < token_end && _is_ws(load8(s, token_start)) {
            token_start += 1
         }
         while token_end > token_start && _is_ws(load8(s, token_end - 1)) {
            token_end -= 1
         }
         if token_end > token_start { count += 1 }
      }
   }
   mut out = list(count)
   scan = start
   while scan < end {
      while scan < end && load8(s, scan) == 32 { scan += 1 }
      if scan < end {
         def int token_start = scan
         while scan < end && load8(s, scan) != 32 { scan += 1 }
         mut int token_end = scan
         while token_start < token_end && _is_ws(load8(s, token_start)) {
            token_start += 1
         }
         while token_end > token_start && _is_ws(load8(s, token_end - 1)) {
            token_end -= 1
         }
         if token_end > token_start {
            out = _list_push_reserved(out, _substr(s, token_start, token_end))
         }
      }
   }
   out
}

@returns_owned
fn word_frequency(any s) dict {
   "Returns a dictionary mapping each split_words token to its occurrence count."
   __dict_word_frequency_raw(s)
}

@returns_owned
fn str_add(any a, any b) str {
   "Concatenates two strings."
   if !is_str(a) { return is_str(b) ? _substr(b, 0, b.len) : to_str(b) }
   if !is_str(b) { return _substr(a, 0, a.len) }
   def n1, n2 = a.len, b.len
   def total = n1 + n2
   mut out = malloc(total + 1)
   if !out { return "" }
   init_str(out, total)
   mut i = 0
   while i < n1 {
      store8(out, load8(a, i), i)
      i += 1
   }
   mut j = 0
   while j < n2 {
      store8(out, load8(b, j), n1 + j)
      j += 1
   }
   store8(out, 0, total)
   return out
}

@returns_owned
fn upper(any s) any {
   "Converts string `s` to uppercase."
   if !is_str(s) { return s }
   mut int n = s.len
   mut out = malloc(n + 1)
   if !out { return "" }
   init_str(out, n)
   mut i = 0
   while i < n {
      mut c = load8(s, i)
      if c >= 97 && c <= 122 { c = c - 32 }
      store8(out, c, i)
      i += 1
   }
   store8(out, 0, n)
   return __cstr_to_str(out)
}

@returns_owned
fn lower(any s) any {
   "Converts string `s` to lowercase."
   if !is_str(s) { return s }
   mut int n = s.len
   mut out = malloc(n + 1)
   if !out { return "" }
   init_str(out, n)
   mut i = 0
   while i < n {
      mut c = load8(s, i)
      if c >= 65 && c <= 90 { c = c + 32 }
      store8(out, c, i)
      i += 1
   }
   store8(out, 0, n)
   return __cstr_to_str(out)
}

fn endswith(any s, any suffix) bool {
   "Returns true if string `s` ends with `suffix`."
   if !is_str(s) || !is_str(suffix) { return false }
   mut n = __str_len(s)
   def int m = __str_len(suffix)
   if n < m { return false }
   return _match_at(s, suffix, n - m)
}

@inline
fn str_contains(any s, any sub) bool {
   "Returns true if string `s` contains `sub`."
   if !is_str(s) || !is_str(sub) { return false }
   ;; Keep this loop on the dynamic string ABI. Passing an exception payload
   ;; through the typed find(str, str) boundary can reinterpret a tagged
   ;; string as a raw pointer after a callback-heavy caller.
   def n = s.len
   def m = sub.len
   if m == 0 { return true }
   if n < m { return false }
   mut int i = 0
   while i + m <= n {
      mut int j = 0
      mut bool same = true
      while j < m {
         if load8(s, i + j) != load8(sub, j) {
            same = false
            break
         }
         j += 1
      }
      if same { return true }
      i += 1
   }
   false
}

@returns_owned
fn join(list items, str sep="") str {
   "Joins a list of strings using separator `sep`."
   if !is_list(items) { return "" }
   if !is_str(sep) { return "" }
   mut n = items.len
   if n == 0 { return "" }
   if n == 1 {
      def one = items.get(0)
      if is_str(one) { return _substr(one, 0, one.len) }
      return to_str(one)
   }
   def sep_len = sep.len
   mut parts = list(n)
   mut total = sep_len * (n - 1)
   mut i = 0
   while i < n {
      mut part = items.get(i)
      if !is_str(part) { part = to_str(part) }
      parts = _list_push_reserved(parts, part)
      total += part.len
      i += 1
   }
   mut out = malloc(total + 1)
   if !out { return "" }
   init_str(out, total)
   mut pos = 0
   i = 0
   while i < n {
      def part = parts.get(i)
      def part_len = part.len
      if part_len > 0 {
         memcpy(out + pos, part, part_len)
         pos += part_len
      }
      if i + 1 < n && sep_len > 0 {
         memcpy(out + pos, sep, sep_len)
         pos += sep_len
      }
      i += 1
   }
   store8(out, 0, total)
   out
}

fn join_words(list items, str sep=" ", int start=0) str {
   "Join list `items` into a string starting from index `start`, using `sep`."
   if !is_list(items) { return "" }
   mut n = items.len
   if start >= n { return "" }
   mut parts = list(n - start)
   mut i = start
   while i < n {
      def part = to_str(items.get(i, ""))
      if part.len > 0 { parts = _list_push_reserved(parts, part) }
      i += 1
   }
   join(parts, sep)
}

@returns_owned
fn str_replace(any s, any old, any new) str {
   "Replaces all occurrences of old in s with new."
   if !is_str(s) || !is_str(old) || !is_str(new) { return is_str(s) ? _substr(s, 0, s.len) : to_str(s) }
   if old.len == 0 { return _substr(s, 0, s.len) }
   def n = s.len
   def old_len = old.len
   def new_len = new.len
   mut count = 0
   mut i = 0
   while i <= n - old_len {
      if _match_at(s, old, i) {
         count += 1
         i += old_len
      } else {
         i += 1
      }
   }
   if count == 0 { return _substr(s, 0, n) }
   def total = n + count * (new_len - old_len)
   mut out = malloc(total + 1)
   if !out { return _substr(s, 0, n) }
   init_str(out, total)
   mut src = 0
   mut dst = 0
   while src <= n - old_len {
      if _match_at(s, old, src) {
         if new_len > 0 {
            memcpy(out + dst, new, new_len)
            dst += new_len
         }
         src += old_len
      } else {
         store8(out, load8(s, src), dst)
         src += 1
         dst += 1
      }
   }
   while src < n {
      store8(out, load8(s, src), dst)
      src += 1
      dst += 1
   }
   store8(out, 0, total)
   out
}

@returns_owned
fn to_hex(int n, int width=0) str {
   "Converts an integer `n` to its hexadecimal string representation."
   def hex_chars = "0123456789abcdef"
   if n == 0 {
      if width <= 1 { return "0" }
      def out = malloc(width + 1)
      if !out { return "" }
      init_str(out, width)
      mut k = 0 while k < width - 1 { store8(out, 48, k) k += 1 }
      store8(out, 48, width - 1)
      store8(out, 0, width)
      return out
   }
   mut val = n
   mut nib = 0
   mut tmp = val
   if tmp < 0 {
      nib = 16
   } else {
      while tmp > 0 { tmp = tmp >> 4 nib += 1 }
   }
   def w = (width > nib) ? width : nib
   if w == 0 { return "" }
   def out = malloc(w + 1)
   if !out { return "" }
   init_str(out, w)
   mut k = 0 while k < w - nib { store8(out, 48, k) k += 1 }
   mut i = w - 1
   mut v2 = val
   while i >= 0 {
      store8(out, load8(hex_chars, v2 & 15), i)
      v2 = v2 >> 4
      i -= 1
   }
   store8(out, 0, w)
   return out
}

@returns_owned
fn to_fixed(number v, int precision=2) str {
   "Returns a string representation of float `v` with `precision` decimal places."
   mut val = v
   mut b = Builder(32)
   if val < 0.0 { b = builder_append(b, "-") val = 0.0 - val }
   def integral = int(val)
   b = builder_append(b, to_str(integral))
   if precision > 0 {
      b = builder_append(b, ".")
      mut frac = val - float(integral)
      mut i = 0
      while i < precision {
         frac = frac * 10.0
         def digit = int(frac)
         b = builder_append(b, to_str(digit))
         frac = frac - float(digit)
         i += 1
      }
   }
   def s = builder_to_str(b)
   builder_free(b)
   s
}

@returns_owned
fn chr(int code) str {
   "Returns a single-character string from an integer Unicode code point."
   if code < 0 || code > 1114111 { return "" }
   def char_buf = malloc(5)
   if !char_buf { return "" }
   mut len = 0
   if code <= 127 {
      store8(char_buf, code, 0)
      len = 1
   } elif code <= 2047 {
      store8(char_buf, (192 | (code >> 6)), 0)
      store8(char_buf, (128 | (code & 63)), 1)
      len = 2
   } elif code <= 65535 {
      store8(char_buf, (224 | (code >> 12)), 0)
      store8(char_buf, (128 | ((code >> 6) & 63)), 1)
      store8(char_buf, (128 | (code & 63)), 2)
      len = 3
   } else {
      store8(char_buf, (240 | (code >> 18)), 0)
      store8(char_buf, (128 | ((code >> 12) & 63)), 1)
      store8(char_buf, (128 | ((code >> 6) & 63)), 2)
      store8(char_buf, (128 | (code & 63)), 3)
      len = 4
   }
   store8(char_buf, 0, len)
   def out = __cstr_to_str(char_buf)
   free(char_buf)
   out
}

@returns_owned
fn repeat(str s, int n) str {
   "Returns string `s` repeated `n` times."
   if !is_str(s) || n < 0 { return "" }
   if n == 0 { return "" }
   def slen = s.len
   def total_len = slen * n
   mut out = malloc(total_len + 1)
   if !out { return "" }
   init_str(out, total_len)
   mut i = 0
   while i < n {
      memcpy(out + i * slen, s, slen)
      i += 1
   }
   store8(out, 0, total_len)
   return out
}

fn _is_utf8_cont(int c) bool {
   def cc = c & 255
   cc >= 128 && cc <= 191
}

fn _utf8_seq_len(str s, int i, int n) int {
   if i < 0 || i >= n { return -1 }
   def b0 = load8(s, i) & 255
   if b0 < 128 { return 1 }
   if b0 >= 194 && b0 <= 223 {
      if i + 1 >= n { return -1 }
      def b1 = load8(s, i + 1) & 255
      if !_is_utf8_cont(b1) { return -1 }
      return 2
   }
   if b0 >= 224 && b0 <= 239 {
      if i + 2 >= n { return -1 }
      def b1, b2 = load8(s, i + 1) & 255, load8(s, i + 2) & 255
      if !_is_utf8_cont(b1) || !_is_utf8_cont(b2) { return -1 }
      if b0 == 224 && b1 < 160 { return -1 }
      if b0 == 237 && b1 >= 160 { return -1 }
      return 3
   }
   if b0 >= 240 && b0 <= 244 {
      if i + 3 >= n { return -1 }
      def b1, b2 = load8(s, i + 1) & 255, load8(s, i + 2) & 255
      def b3 = load8(s, i + 3) & 255
      if !_is_utf8_cont(b1) || !_is_utf8_cont(b2) || !_is_utf8_cont(b3) { return -1 }
      if b0 == 240 && b1 < 144 { return -1 }
      if b0 == 244 && b1 > 143 { return -1 }
      return 4
   }
   -1
}

fn _utf8_decode_at(str s, int i, int w) int {
   def b0 = load8(s, i) & 255
   if w == 1 { return b0 }
   if w == 2 {
      def b1 = load8(s, i + 1) & 255
      return (((b0 & 31) << 6) | (b1 & 63))
   }
   if w == 3 {
      def b1, b2 = load8(s, i + 1) & 255, load8(s, i + 2) & 255
      return ((((b0 & 15) << 12) | ((b1 & 63) << 6)) | (b2 & 63))
   }
   if w == 4 {
      def b1, b2 = load8(s, i + 1) & 255, load8(s, i + 2) & 255
      def b3 = load8(s, i + 3) & 255
      return (((((b0 & 7) << 18) | ((b1 & 63) << 12)) | ((b2 & 63) << 6)) | (b3 & 63))
   }
   0
}

fn _utf8_encode_at(ptr p, int i, int code) int {
   if code < 0 || code > 1114111 { return 0 }
   if code <= 127 {
      store8(p, code, i)
      return 1
   } elif code <= 2047 {
      store8(p, (192 | (code >> 6)), i)
      store8(p, (128 | (code & 63)), i + 1)
      return 2
   } elif code <= 65535 {
      store8(p, (224 | (code >> 12)), i)
      store8(p, (128 | ((code >> 6) & 63)), i + 1)
      store8(p, (128 | (code & 63)), i + 2)
      return 3
   } else {
      store8(p, (240 | (code >> 18)), i)
      store8(p, (128 | ((code >> 12) & 63)), i + 1)
      store8(p, (128 | ((code >> 6) & 63)), i + 2)
      store8(p, (128 | (code & 63)), i + 3)
      return 4
   }
}

fn utf8_valid(any s) bool {
   "Returns true if `s` is valid UTF-8."
   if !is_str(s) { return false }
   def int n = s.len
   mut i = 0
   while i < n {
      def w = _utf8_seq_len(s, i, n)
      if w < 1 { return false }
      i = i + w
   }
   true
}

fn _utf8_len_str(str source) int {
   def int n = source.len
   mut i = 0
   mut count = 0
   while i < n {
      def w = _utf8_seq_len(source, i, n)
      if w < 1 { i += 1 }
      else { i = i + w }
      count += 1
   }
   count
}

fn utf8_len(any s) int {
   "Returns the number of UTF-8 code points in `s` (invalid bytes count as one)."
   if !is_str(s) && !__is_str_obj(s) { return 0 }
   def str source = to_str(s)
   _utf8_len_str(source)
}

fn ord_at(any s, int idx=0) int {
   "Returns Unicode code point at code-point index `idx` (supports negative indices)."
   if !is_str(s) && !__is_str_obj(s) { return 0 }
   ;; Normalize once at the dynamic-to-typed boundary. Re-forwarding an `any`
   ;; local through nested UTF-8 helpers lets a stale dynamic carrier fact leak
   ;; into the next call ABI after earlier string allocations.
   def str source = to_str(s)
   mut total = _utf8_len_str(source)
   if idx < 0 { idx = total + idx }
   if idx < 0 || idx >= total { return 0 }
   def int n = source.len
   mut i = 0
   mut pos = 0
   while i < n {
      def w = _utf8_seq_len(source, i, n)
      if pos == idx {
         if w < 1 { return load8(source, i) }
         return _utf8_decode_at(source, i, w)
      }
      if w < 1 { i += 1 }
      else { i = i + w }
      pos += 1
   }
   0
}

@inline
fn ord(any s) int {
   "Returns the Unicode code point of the first character in `s`."
   ord_at(s, 0)
}

fn byte_at(any s, int idx=0, int default=0) int {
   "Returns raw byte at byte index `idx`, supporting negative indices."
   def int n = __str_len(s)
   if idx < 0 { idx = n + idx }
   if idx < 0 || idx >= n { return default }
   load8(s, idx)
}

@inline
fn ascii_is_lower(int c) bool {
   "Returns true for ASCII lowercase bytes."
   c >= 97 && c <= 122
}

@inline
fn ascii_is_upper(int c) bool {
   "Returns true for ASCII uppercase bytes."
   c >= 65 && c <= 90
}

@inline
fn ascii_is_alpha(int c) bool {
   "Returns true for ASCII letters."
   ascii_is_lower(c) || ascii_is_upper(c)
}

@inline
fn ascii_is_digit(int c) bool {
   "Returns true for ASCII decimal digits."
   c >= 48 && c <= 57
}

@inline
fn ascii_is_alnum(int c) bool {
   "Returns true for ASCII letters or digits."
   ascii_is_alpha(c) || ascii_is_digit(c)
}

@inline
fn ascii_is_space(int c) bool {
   "Returns true for ASCII whitespace bytes."
   _is_ws(c)
}

@inline
fn ascii_is_hex_digit(int c) bool {
   "Returns true for ASCII hexadecimal digits."
   ascii_is_digit(c) || (c >= 65 && c <= 70) || (c >= 97 && c <= 102)
}

@inline
fn ascii_is_oct_digit(int c) bool {
   "Returns true for ASCII octal digits."
   c >= 48 && c <= 55
}

@inline
fn ascii_is_punctuation(int c) bool {
   "Returns true for printable ASCII punctuation."
   (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126)
}

@inline
fn ascii_is_printable(int c) bool {
   "Returns true for printable ASCII bytes."
   c >= 32 && c <= 126
}

@inline
fn ascii_lower_byte(int c) int {
   "Lowercases one ASCII byte when possible."
   if ascii_is_upper(c) { return c + 32 }
   c
}

@inline
fn ascii_upper_byte(int c) int {
   "Uppercases one ASCII byte when possible."
   if ascii_is_lower(c) { return c - 32 }
   c
}

fn ascii_only(str s) bool {
   "Returns true when all bytes in `s` are ASCII."
   mut i = 0
   while i < s.len {
      if load8(s, i) > 127 { return false }
      i += 1
   }
   true
}

fn ascii_all_alpha(str s) bool {
   "Returns true when every byte is an ASCII letter."
   if s.len == 0 { return false }
   mut i = 0
   while i < s.len {
      if !ascii_is_alpha(load8(s, i)) { return false }
      i += 1
   }
   true
}

fn ascii_all_digit(str s) bool {
   "Returns true when every byte is an ASCII decimal digit."
   if s.len == 0 { return false }
   mut i = 0
   while i < s.len {
      if !ascii_is_digit(load8(s, i)) { return false }
      i += 1
   }
   true
}

fn ascii_all_alnum(str s) bool {
   "Returns true when every byte is ASCII alphanumeric."
   if s.len == 0 { return false }
   mut i = 0
   while i < s.len {
      if !ascii_is_alnum(load8(s, i)) { return false }
      i += 1
   }
   true
}

fn ascii_all_space(str s) bool {
   "Returns true when every byte is ASCII whitespace."
   if s.len == 0 { return false }
   mut i = 0
   while i < s.len {
      if !ascii_is_space(load8(s, i)) { return false }
      i += 1
   }
   true
}

fn ascii_all_printable(str s) bool {
   "Returns true when every byte is printable ASCII."
   mut i = 0
   while i < s.len {
      if !ascii_is_printable(load8(s, i)) { return false }
      i += 1
   }
   true
}

impl int {
   @inline
   fn ascii_lower(self c) int { ascii_lower_byte(c) }
   @inline
   fn ascii_upper(self c) int { ascii_upper_byte(c) }
   @inline
   fn ascii_is_lower(self c) bool { ascii_is_lower(c) }
   @inline
   fn ascii_is_upper(self c) bool { ascii_is_upper(c) }
   @inline
   fn ascii_is_alpha(self c) bool { ascii_is_alpha(c) }
   @inline
   fn ascii_is_digit(self c) bool { ascii_is_digit(c) }
   @inline
   fn ascii_is_alnum(self c) bool { ascii_is_alnum(c) }
   @inline
   fn ascii_is_space(self c) bool { ascii_is_space(c) }
   @inline
   fn ascii_is_hex_digit(self c) bool { ascii_is_hex_digit(c) }
   @inline
   fn ascii_is_oct_digit(self c) bool { ascii_is_oct_digit(c) }
   @inline
   fn ascii_is_punctuation(self c) bool { ascii_is_punctuation(c) }
   @inline
   fn ascii_is_printable(self c) bool { ascii_is_printable(c) }
}

impl str {
   @inline
   fn find(self s, str sub) int { find(s, sub) }
   @inline
   fn find_from(self s, str sub, int start) int { find_from(s, sub, start) }
   @inline
   fn find_last(self s, str sub) int { find_last(s, sub) }
   @inline
   fn contains(self s, str sub) bool { str_contains(s, sub) }
   @inline
   fn startswith(self s, str prefix) bool { startswith(s, prefix) }
   @inline
   fn endswith(self s, str suffix) bool { endswith(s, suffix) }
   @inline
   fn strip(self s) str { strip(s) }
   @inline
   fn split(self s, str sep) list { _split_impl(s, sep) }
   @inline
   fn split_words(self s) list { split_words(s) }
   @inline
   fn word_frequency(self s) dict { word_frequency(s) }
   @inline
   fn upper(self s) str { upper(s) }
   @inline
   fn lower(self s) str { lower(s) }
   @inline
   fn replace(self s, str old, str new) str { str_replace(s, old, new) }
   @inline
   fn pad_start(self s, int width, str pad=" ") str { pad_start(s, width, pad) }
   @inline
   fn repeat(self s, int n) str { repeat(s, n) }
   @inline
   fn atoi(self s) int { atoi(s) }
   @inline
   fn parse_int(self s, int base=10) int { parse_int(s, base) }
   @inline
   fn atof(self s) f64 { atof(s) }
   @inline
   fn ord(self s) int { ord(s) }
   @inline
   fn ord_at(self s, int idx=0) int { ord_at(s, idx) }
   @inline
   fn byte_at(self s, int idx=0, int default=0) int {
      ;; Qualify the module-level helper: the attached overload has this same
      ;; leaf name and a different dynamic ABI, so an unqualified call can
      ;; resolve to this overload instead.
      core.str.byte_at(s, idx, default)
   }
   @inline
   fn utf8_valid(self s) bool { utf8_valid(s) }
   @inline
   fn utf8_len(self s) int { utf8_len(s) }
   @inline
   fn str_slice(self s, int start, int stop, int step=1) str { _str_slice(s, start, stop, step) }
   @inline
   fn utf8_slice(self s, int start, int stop, int step=1) str { _utf8_slice(s, start, stop, step) }
   @inline
   fn ascii_only(self s) bool { ascii_only(s) }
   @inline
   fn ascii_alpha(self s) bool { ascii_all_alpha(s) }
   @inline
   fn ascii_digit(self s) bool { ascii_all_digit(s) }
   @inline
   fn ascii_alnum(self s) bool { ascii_all_alnum(s) }
   @inline
   fn ascii_space(self s) bool { ascii_all_space(s) }
   @inline
   fn ascii_printable(self s) bool { ascii_all_printable(s) }
   operator * int: str = repeat
}

fn str_slice(str s, int start, int stop, int step=1) str {
   "Returns a slice of string `s` from `start` to `stop` with optional `step`."
   return _str_slice(s, start, stop, step)
}

@returns_owned
fn utf8_slice(str s, int start, int stop, int step=1) str {
   "Returns a UTF-8 code-point slice of string `s`."
   return _utf8_slice(s, start, stop, step)
}

@returns_owned
fn Builder(int initial_cap=64) ptr {
   "Creates a new StringBuilder with initial capacity."
   __str_builder_new(initial_cap)
}

@returns_owned
@consumes(b)
fn builder_append(ptr b, any s) ptr {
   "Appends string `s` to the builder `b`."
   if !is_str(s) {
      if is_ptr(s) { s = __cstr_to_str(s) }
      else { s = to_str(s) }
   }
   __str_builder_append(b, s)
}

@returns_owned
@consumes(b)
fn builder_append_byte(ptr b, int c) ptr {
   "Appends one raw byte to the builder `b`."
   __str_builder_append_byte(b, c & 255)
}

@returns_owned
fn builder_to_str(ptr b) str {
   "Returns the content of builder `b` as a Nytrix string."
   __str_builder_to_str(b)
}

fn builder_free(ptr b) int {
   "Frees the underlying buffer of builder `b`."
   __str_builder_free(b)
}

fn _str_slice(str s, int start, int stop, int step=1) str {
   "Returns a slice of string `s` from `start` to `stop` with optional `step`."
   if !is_str(s) { return "" }
   if step == 0 { step = 1 }
   slice(s, start, stop, step)
}

@returns_owned
fn _utf8_slice(str s, int start, int stop, int step=1) str {
   "Returns a UTF-8 code-point slice of string `s`."
   if !is_str(s) { return "" }
   if step == 0 { step = 1 }
   def n = utf8_len(s)
   if start < 0 { start = n + start }
   if stop < 0 { stop = n + stop }
   if step > 0 {
      if start < 0 { start = 0 }
      if stop > n { stop = n }
      if start >= stop { return "" }
      if step == 1 && start == 0 && stop == n { return clone(s) }
   } else {
      if start >= n { start = n - 1 }
      if stop < -1 { stop = -1 }
      if start <= stop { return "" }
   }
   mut count = 0
   mut t = start
   if step > 0 {
      while t < stop {
         count += 1
         t = t + step
      }
   } else {
      while t > stop {
         count += 1
         t = t + step
      }
   }
   ;; Keep the handle and scalar index in separate bindings. Besides making
   ;; their ABI classes explicit to native lowering, this avoids letting the
   ;; tuple binder merge the Builder pointer with the integer loop index.
   mut ptr b = Builder(count * 4 + 8)
   if step > 0 {
      ;; Convert the starting code-point index to a byte offset once, then
      ;; walk forward. Calling ord_at(s, i) for each output character rescans
      ;; the string from byte zero and makes contiguous slices quadratic.
      def int byte_len = s.len
      mut int byte_index = 0
      mut int code_index = 0
      while byte_index < byte_len && code_index < start {
         def width = _utf8_seq_len(s, byte_index, byte_len)
         if width < 1 { byte_index += 1 }
         else { byte_index += width }
         code_index += 1
      }
      mut int i = start
      while i < stop {
         def width = _utf8_seq_len(s, byte_index, byte_len)
         def code = width < 1 ? load8(s, byte_index) :
         _utf8_decode_at(s, byte_index, width)
         b = builder_append(b, chr(code))
         mut int skipped = 0
         while skipped < step && byte_index < byte_len {
            def next_width = _utf8_seq_len(s, byte_index, byte_len)
            if next_width < 1 { byte_index += 1 }
            else { byte_index += next_width }
            skipped += 1
         }
         i += step
      }
   } else {
      mut int i = start
      while i > stop {
         b = builder_append(b, chr(ord_at(s, i)))
         i += step
      }
   }
   def out = builder_to_str(b)
   builder_free(b)
   return out
}

#main {
   assert(__flt_is_nan(atof("nan")) && __flt_is_nan(atof("-NaN")), "str atof nan")
   assert(__flt_is_inf(atof("inf")) && __flt_is_inf(atof("+Infinity")) && atof("-inf") < 0.0, "str atof infinity")
   assert(atof("0.5") == 0.5 && atof("-12.25") == -12.25 && atof("1e3") == 1000.0, "str atof decimal")
   assert(split("éa", "") == ["é", "a"], "str split empty sep utf8")
   mut ptr utf8_builder = Builder(4)
   utf8_builder = builder_append_byte(utf8_builder, 0xc3)
   utf8_builder = builder_append_byte(utf8_builder, 0xa9)
   def str utf8_bytes = builder_to_str(utf8_builder)
   builder_free(utf8_builder)
   assert(utf8_bytes == "é", "str builder append preserves raw UTF-8 bytes")
   assert(join_words(["a", "", "b"], "-") == "a-b", "str join_words")
   print("✓ core.str self-test passed")
}
