;; Keywords: data serialization json parse
;; JavaScript Object Notation (JSON) Parser and Generator for Nytrix
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc8259.html
;; References:
;; - std.math.parse.data
;; - std.math.parse
module std.math.parse.data.json(json_decode, json_try_decode, json_last_error, json_encode)
use std.core
use std.core.dict_mod
use std.core.str as str
use std.math.float as f

mut _json_error = ""

fn json_last_error() str {
   "Returns the error from the last decode attempt(empty string on success)."
   _json_error
}

fn _json_set_error(list st, str msg) int {
   def cur = st.get(3, "")
   if !is_str(cur) || cur.len == 0 { st[3] = msg }
   0
}

fn _json_make_result(bool ok, any value, str err, int pos) dict {
   mut r = dict(8)
   r["ok"] = ok
   r["value"] = value
   r["error"] = err
   r["pos"] = pos
   r
}

fn _json_peek(list st) int {
   def pos = st.get(2)
   def n = st.get(1)
   if pos < 0 || pos >= n { return -1 }
   load8(st.get(0), pos)
}

@inline
fn _json_is_ws(int c) bool {
   return case c {
      9, 10, 13, 32 -> true
      _ -> false
   }
}

fn _json_skip_ws(list st) any {
   def s, n = st.get(0), st.get(1)
   mut pos = st.get(2)
   def start = pos
   while pos < n {
      if _json_is_ws(load8(s, pos)) { pos += 1 } else { break }
   }
   if pos != start { st[2] = pos }
}

fn _json_expect(list st, int want, str msg) any {
   def pos = st.get(2)
   def n = st.get(1)
   def c = (pos >= 0 && pos < n) ? load8(st.get(0), pos) : -1
   if c != want { return _json_set_error(st, msg) }
   st[2] = pos + 1
   true
}

fn _json_is_digit(int c) bool { c >= 48 && c <= 57 }

fn _json_hex4(str s, int start) int {
   mut cp = 0
   mut k = 0
   while k < 4 {
      def hv = str.hex_val(load8(s, start + k))
      if hv < 0 { return -1 }
      cp = cp * 16 + hv
      k += 1
   }
   cp
}

fn _json_parse_literal(list st, str lit, any value) any {
   def s, n = st.get(0), st.get(1)
   def pos = st.get(2)
   def m = lit.len
   if pos + m > n { return _json_set_error(st, "unexpected end while parsing literal") }
   mut i = 0
   while i < m {
      if load8(s, pos + i) != load8(lit, i) { return _json_set_error(st, "invalid literal") }
      i += 1
   }
   st[2] = pos + m
   value
}

fn _json_parse_float_text(str s) f64 { str.atof(s) }

fn _json_parse_val(list st) any {
   _json_skip_ws(st)
   def pos = st.get(2)
   def n = st.get(1)
   def c = (pos >= 0 && pos < n) ? load8(st.get(0), pos) : -1
   if c < 0 { return _json_set_error(st, "unexpected end of input") }
   return case c {
      123 -> _json_parse_obj(st)
      91 -> _json_parse_arr(st)
      34 -> _json_parse_str(st)
      116 -> _json_parse_literal(st, "true", true)
      102 -> _json_parse_literal(st, "false", false)
      110 -> _json_parse_literal(st, "null", 0)
      45, 48..57 -> _json_parse_num(st)
      _ -> _json_set_error(st, "unexpected token")
   }
}

fn _json_parse_obj(list st) any {
   if !_json_expect(st, 123, "expected '{'") { return 0 }
   mut d = dict(8)
   _json_skip_ws(st)
   if _json_peek(st) == 125 {
      st[2] = st.get(2) + 1
      return d
   }
   while 1 {
      _json_skip_ws(st)
      if _json_peek(st) != 34 { return _json_set_error(st, "expected string key") }
      def key = _json_parse_str(st)
      if len(st.get(3, "")) > 0 { return 0 }
      _json_skip_ws(st)
      if !_json_expect(st, 58, "expected ':' after object key") { return 0 }
      def val = _json_parse_val(st)
      if len(st.get(3, "")) > 0 { return 0 }
      d[key] = val
      _json_skip_ws(st)
      def c = _json_peek(st)
      if c == 44 {
         st[2] = st.get(2) + 1
         continue
      }
      if c == 125 {
         st[2] = st.get(2) + 1
         return d
      }
      return _json_set_error(st, "expected ',' or '}' in object")
   }
}

fn _json_parse_arr(list st) any {
   if !_json_expect(st, 91, "expected '['") { return 0 }
   mut l = list(8)
   _json_skip_ws(st)
   if _json_peek(st) == 93 {
      st[2] = st.get(2) + 1
      return l
   }
   while 1 {
      l = l.append(_json_parse_val(st))
      if len(st.get(3, "")) > 0 { return 0 }
      _json_skip_ws(st)
      def c = _json_peek(st)
      if c == 44 {
         st[2] = st.get(2) + 1
         continue
      }
      if c == 93 {
         st[2] = st.get(2) + 1
         return l
      }
      return _json_set_error(st, "expected ',' or ']' in array")
   }
}

fn _json_parse_str(list st) any {
   mut pos = st.get(2)
   def s, n = st.get(0), st.get(1)
   if pos >= n || load8(s, pos) != 34 { return _json_set_error(st, "expected string") }
   pos += 1
   mut end = pos
   mut has_esc = false
   while end < n {
      def c = load8(s, end)
      if c >= 0 && c < 32 { return _json_set_error(st, "invalid control character in string") }
      if c == 34 { break }
      if c == 92 {
         has_esc = true
         end += 2
      } else {
         end += 1
      }
   }
   if end >= n { return _json_set_error(st, "unterminated string") }
   if has_esc == false {
      def out_len = end - pos
      def out = malloc(out_len + 1)
      if !out { return _json_set_error(st, "string allocation failed") }
      init_str(out, out_len)
      if out_len > 0 { __copy_mem(out, s + pos, out_len) }
      st[2] = end + 1
      return out
   }
   def out = malloc(end - pos + 1)
   if !out { return _json_set_error(st, "string allocation failed") }
   mut out_len = 0
   mut cur = pos
   while cur < end {
      def c = load8(s, cur)
      if c == 92 {
         cur += 1
         if cur >= end { return _json_set_error(st, "unterminated escape sequence") }
         def esc = load8(s, cur)
         if esc == 34 {
            store8(out, 34, out_len)
            out_len += 1
         } elif esc == 92 {
            store8(out, 92, out_len)
            out_len += 1
         } elif esc == 47 {
            store8(out, 47, out_len)
            out_len += 1
         } elif esc == 98 {
            store8(out, 8, out_len)
            out_len += 1
         } elif esc == 102 {
            store8(out, 12, out_len)
            out_len += 1
         } elif esc == 110 {
            store8(out, 10, out_len)
            out_len += 1
         } elif esc == 114 {
            store8(out, 13, out_len)
            out_len += 1
         } elif esc == 116 {
            store8(out, 9, out_len)
            out_len += 1
         } elif esc == 117 {
            if cur + 4 >= end { return _json_set_error(st, "invalid unicode escape") }
            mut cp1 = _json_hex4(s, cur + 1)
            if cp1 < 0 { return _json_set_error(st, "invalid unicode escape") }
            cur = cur + 4
            mut cp = cp1
            if cp1 >= 55296 && cp1 <= 56319 {
               if cur + 6 >= end { return _json_set_error(st, "invalid unicode surrogate pair") }
               if load8(s, cur + 1) != 92 || load8(s, cur + 2) != 117 { return _json_set_error(st, "invalid unicode surrogate pair") }
               def cp2 = _json_hex4(s, cur + 3)
               if cp2 < 56320 || cp2 > 57343 { return _json_set_error(st, "invalid unicode surrogate pair") }
               cp = 65536 + ((cp1 - 55296) * 1024) + (cp2 - 56320)
               cur = cur + 6
            }
            if cp1 >= 56320 && cp1 <= 57343 { return _json_set_error(st, "invalid unicode surrogate pair") }
            if cp < 128 {
               store8(out, cp, out_len)
               out_len += 1
            } elif cp < 2048 {
               store8(out, 192 | (cp >> 6), out_len)
               out_len += 1
               store8(out, 128 | (cp & 63), out_len)
               out_len += 1
            } elif cp < 65536 {
               store8(out, 224 | (cp >> 12), out_len)
               out_len += 1
               store8(out, 128 | ((cp >> 6) & 63), out_len)
               out_len += 1
               store8(out, 128 | (cp & 63), out_len)
               out_len += 1
            } else {
               store8(out, 240 | (cp >> 18), out_len)
               out_len += 1
               store8(out, 128 | ((cp >> 12) & 63), out_len)
               out_len += 1
               store8(out, 128 | ((cp >> 6) & 63), out_len)
               out_len += 1
               store8(out, 128 | (cp & 63), out_len)
               out_len += 1
            }
         } else {
            return _json_set_error(st, "invalid escape sequence")
         }
      } else {
         if c >= 0 && c < 32 { return _json_set_error(st, "invalid control character in string") }
         store8(out, c, out_len)
         out_len += 1
      }
      cur += 1
   }
   def result = init_str(out, out_len)
   st[2] = end + 1
   result
}

fn _json_parse_num(list st) any {
   mut pos = st.get(2)
   def s, n = st.get(0), st.get(1)
   mut start = pos
   mut neg = false
   if pos < n && load8(s, pos) == 45 {
      neg = true
      pos += 1
   }
   if pos >= n { return _json_set_error(st, "invalid number") }
   mut int_val = 0
   if load8(s, pos) == 48 {
      pos += 1
      if pos < n && _json_is_digit(load8(s, pos)) { return _json_set_error(st, "leading zero in number") }
   } elif _json_is_digit(load8(s, pos)) {
      while pos < n && _json_is_digit(load8(s, pos)) {
         int_val = int_val * 10 + (load8(s, pos) - 48)
         pos += 1
      }
   } else {
      return _json_set_error(st, "invalid number")
   }
   mut has_frac = false
   mut has_exp = false
   if pos < n && load8(s, pos) == 46 {
      has_frac = true
      pos += 1
      if pos >= n || !_json_is_digit(load8(s, pos)) { return _json_set_error(st, "invalid fraction in number") }
      while pos < n && _json_is_digit(load8(s, pos)) { pos += 1 }
   }
   if pos < n && (load8(s, pos) == 101 || load8(s, pos) == 69) {
      has_exp = true
      pos += 1
      if pos < n && (load8(s, pos) == 43 || load8(s, pos) == 45) { pos += 1 }
      if pos >= n || !_json_is_digit(load8(s, pos)) { return _json_set_error(st, "invalid exponent in number") }
      while pos < n && _json_is_digit(load8(s, pos)) { pos += 1 }
   }
   if !has_frac && !has_exp {
      st[2] = pos
      return neg ? (0 - int_val) : int_val
   }
   def len = pos - start
   def raw = malloc(len + 1)
   if !raw { return _json_set_error(st, "number allocation failed") }
   def tmp = raw
   __copy_mem(tmp, s + start, len)
   store8(tmp, 0, len)
   init_str(tmp, len)
   mut res = 0
   if has_frac || has_exp { res = _json_parse_float_text(tmp) }
   else { res = str.atoi(tmp) }
   st[2] = pos
   free(raw)
   return res
}

fn json_try_decode(any s) dict {
   "Decodes JSON and returns `{ok, value, error, pos}`."
   if !is_str(s) {
      _json_error = "json input must be a string"
      return _json_make_result(false, 0, _json_error, 0)
   }
   def st = [s, s.len, 0, ""]
   def val = _json_parse_val(st)
   _json_skip_ws(st)
   mut err = st.get(3, "")
   if err.len == 0 && st.get(2) != st.get(1) {
      err = "trailing characters after JSON value"
      st[3] = err
   }
   _json_error = st.get(3, "")
   if _json_error.len == 0 { return _json_make_result(true, val, "", st.get(2)) }
   _json_make_result(false, 0, _json_error, st.get(2))
}

fn json_decode(any s) any {
   "Decodes JSON string and returns parsed value(`0` on error)."
   def res = json_try_decode(s)
   if res != 0 && res.get("ok", false) { return res.get("value", 0) }
   0
}

fn _json_hex_digit(int n) str {
   if n < 10 { return chr(48 + n) }
   chr(87 + n)
}

fn _json_escape_string(any s) str {
   if !is_str(s) { return "\"\"" }
   def n = s.len
   mut out = Builder(max(16, n + 8))
   out = builder_append(out, "\"")
   mut i = 0
   while i < n {
      def c = load8(s, i)
      match c {
         34 -> { out = builder_append(out, "\\\"") }
         92 -> { out = builder_append(out, "\\\\") }
         8 -> { out = builder_append(out, "\\b") }
         12 -> { out = builder_append(out, "\\f") }
         10 -> { out = builder_append(out, "\\n") }
         13 -> { out = builder_append(out, "\\r") }
         9 -> { out = builder_append(out, "\\t") }
         _ -> {
            if c < 32 {
               out = builder_append(out, "\\u00")
               out = builder_append(out, _json_hex_digit((c / 16) % 16))
               out = builder_append(out, _json_hex_digit(c % 16))
            } else {
               out = builder_append_byte(out, c)
            }
         }
      }
      i += 1
   }
   out = builder_append(out, "\"")
   def s_out = builder_to_str(out)
   builder_free(out)
   s_out
}

fn _json_encode_seq(any v) str {
   def n = v.len
   mut out = Builder(max(16, n * 8 + 8))
   out = builder_append(out, "[")
   mut i = 0
   while i < n {
      out = builder_append(out, json_encode(v.get(i)))
      if i + 1 < n { out = builder_append(out, ",") }
      i += 1
   }
   out = builder_append(out, "]")
   def s_out = builder_to_str(out)
   builder_free(out)
   s_out
}

fn json_encode(any obj) str {
   "Encodes Nytrix values into JSON string."
   if type(obj) == "bool" {
      if obj { return "true" }
      return "false"
   }
   if obj == 0 { return "null" }
   if is_int(obj) { return to_str(obj) }
   if f.is_float(obj) { return to_str(obj) }
   if is_str(obj) { return _json_escape_string(obj) }
   if is_list(obj) || is_tuple(obj) || is_set(obj) { return _json_encode_seq(obj) }
   if is_dict(obj) {
      def items = dict_items(obj)
      mut i = 0
      def n = items.len
      mut out = Builder(max(16, n * 12 + 8))
      out = builder_append(out, "{")
      while i < n {
         def pair = items.get(i)
         def k = pair.get(0)
         def v = pair.get(1)
         mut key = ""
         if is_str(k) { key = k }
         else { key = to_str(k) }
         out = builder_append(out, _json_escape_string(key))
         out = builder_append(out, ":")
         out = builder_append(out, json_encode(v))
         if i + 1 < n { out = builder_append(out, ",") }
         i += 1
      }
      out = builder_append(out, "}")
      def s_out = builder_to_str(out)
      builder_free(out)
      return s_out
   }
   "null"
}

#main {
   def raw = "{\"name\":\"ny\",\"version\":\"0.5.0\",\"tags\":[\"fast\",\"compiled\",\"llvm\"],\"meta\":{\"active\":true,\"score\":9}}"
   def parsed = json_decode(raw)
   assert_eq(parsed.get("name"), "ny", "json object string")
   assert_eq(parsed.get("version"), "0.5.0", "json object version")
   assert_eq(parsed.get("tags").get(2), "llvm", "json array")
   assert_eq(parsed.get("meta").get("score"), 9, "json nested object")
   def back = json_encode(parsed)
   assert(str_contains(back, "\"name\""), "json encoded name")
   assert(str_contains(back, "\"tags\""), "json encoded tags")
   print("✓ std.math.parse.data.json self-test passed")
}
