;; Keywords: data serialization json parse
;; JavaScript Object Notation (JSON) Parser and Generator for Nytrix
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc8259.html
;; References:
;; - core.math.parse.data
;; - core.math.parse
module core.math.parse.data.json(json_decode, json_try_decode,
   json_try_decode_bounded, json_last_error,
   json_encode)
use core
use core.dict_mod
use core.str as str
use core.math.float as f

mut _json_error = ""

;; Keep source, byte size, cursor, and error in separate logical list slots.
fn _json_pos(list st) int { int(st.get(4, 0)) }

fn _json_size(list st) int { int(st.get(3, 0)) }

fn _json_set_pos(list st, int pos) int {
   __store_item_fast(st, 4, pos)
   0
}

fn json_last_error() str {
   "Returns the error from the last decode attempt(empty string on success)."
   _json_error
}

fn _json_set_error(list st, str msg) int {
   def cur = st.get(5, "")
   if !is_str(cur) || cur.len == 0 { __store_item_fast(st, 5, msg) }
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
   def int pos = _json_pos(st)
   def int n = _json_size(st)
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
   def s = st.get(0)
   def int n = _json_size(st)
   mut int pos = _json_pos(st)
   def start = pos
   while pos < n {
      if _json_is_ws(load8(s, pos)) { pos += 1 } else { break }
   }
   if pos != start { _json_set_pos(st, pos) }
}

fn _json_expect(list st, int want, str msg) any {
   def int pos = _json_pos(st)
   def int n = _json_size(st)
   def c = (pos >= 0 && pos < n) ? load8(st.get(0), pos) : -1
   if c != want { return _json_set_error(st, msg) }
   _json_set_pos(st, pos + 1)
   true
}

fn _json_is_digit(int c) bool { c >= 48 && c <= 57 }

fn _json_hex4(str s, int start) int {
   mut int cp = 0
   mut int k = 0
   while k < 4 {
      def hv = str.hex_val(load8(s, start + k))
      if hv < 0 { return -1 }
      cp = cp * 16 + hv
      k += 1
   }
   cp
}

fn _json_parse_literal(list st, str lit, any value) any {
   def s = st.get(0)
   def int n = _json_size(st)
   def int pos = _json_pos(st)
   def m = lit.len
   if pos + m > n { return _json_set_error(st, "unexpected end while parsing literal") }
   mut i = 0
   while i < m {
      if load8(s, pos + i) != load8(lit, i) { return _json_set_error(st, "invalid literal") }
      i += 1
   }
   _json_set_pos(st, pos + m)
   value
}

fn _json_parse_float_text(str s) f64 { str.atof(s) }

fn _json_parse_val(list st, int depth) any {
   _json_skip_ws(st)
   def int pos = _json_pos(st)
   def int n = _json_size(st)
   def c = (pos >= 0 && pos < n) ? load8(st.get(0), pos) : -1
   if c < 0 { return _json_set_error(st, "unexpected end of input") }
   if c == 123 || c == 91 {
      def max_depth = int(st.get(6, 0))
      if max_depth > 0 && depth >= max_depth {
         return _json_set_error(st, "JSON nesting limit exceeded")
      }
      if c == 123 { return _json_parse_obj(st, depth + 1) }
      return _json_parse_arr(st, depth + 1)
   }
   if c == 34 { return _json_parse_str(st) }
   if c == 116 { return _json_parse_literal(st, "true", true) }
   if c == 102 { return _json_parse_literal(st, "false", false) }
   if c == 110 { return _json_parse_literal(st, "null", nil) }
   if c == 45 || _json_is_digit(c) { return _json_parse_num(st) }
   _json_set_error(st, "unexpected token")
   ""
}

fn _json_parse_obj(list st, int depth) any {
   if !_json_expect(st, 123, "expected '{'") { return 0 }
   mut d = dict(8)
   _json_skip_ws(st)
   if _json_peek(st) == 125 {
      _json_set_pos(st, _json_pos(st) + 1)
      return d
   }
   while 1 {
      _json_skip_ws(st)
      if _json_peek(st) != 34 { return _json_set_error(st, "expected string key") }
      def key = _json_parse_str(st)
      if len(st.get(5, "")) > 0 { return 0 }
      _json_skip_ws(st)
      if !_json_expect(st, 58, "expected ':' after object key") { return 0 }
      def val = _json_parse_val(st, depth)
      if len(st.get(5, "")) > 0 { return 0 }
      d = dict_set(d, key, val)
      _json_skip_ws(st)
      def c = _json_peek(st)
      if c == 44 {
         _json_set_pos(st, _json_pos(st) + 1)
         continue
      }
      if c == 125 {
         _json_set_pos(st, _json_pos(st) + 1)
         return d
      }
      return _json_set_error(st, "expected ',' or '}' in object")
   }
}

fn _json_parse_arr(list st, int depth) any {
   if !_json_expect(st, 91, "expected '['") { return 0 }
   mut l = list(8)
   _json_skip_ws(st)
   if _json_peek(st) == 93 {
      _json_set_pos(st, _json_pos(st) + 1)
      return l
   }
   while 1 {
      l = l.append(_json_parse_val(st, depth))
      if len(st.get(5, "")) > 0 { return 0 }
      _json_skip_ws(st)
      def c = _json_peek(st)
      if c == 44 {
         _json_set_pos(st, _json_pos(st) + 1)
         continue
      }
      if c == 93 {
         _json_set_pos(st, _json_pos(st) + 1)
         return l
      }
      return _json_set_error(st, "expected ',' or ']' in array")
   }
}

@inline
fn _json_parse_str(list st) str {
   mut int pos = _json_pos(st)
   def str s = st.get(0)
   def int n = _json_size(st)
   if pos >= n || load8(s, pos) != 34 {
      _json_set_error(st, "expected string")
      return ""
   }
   pos += 1
   mut int scan_pos = pos
   mut has_esc = false
   while scan_pos < n {
      def c = load8(s, scan_pos)
      if c >= 0 && c < 32 {
         _json_set_error(st, "invalid control character in string")
         return ""
      }
      if c == 34 { break }
      if c == 92 {
         has_esc = true
         ;; Skip the escaped byte so an escaped quote does not terminate the
         ;; string. Unicode escape digits are scanned normally after `u`.
         scan_pos += 2
      } else {
         scan_pos += 1
      }
   }
   if scan_pos >= n {
      _json_set_error(st, "unterminated string")
      return ""
   }
   if has_esc == false {
      def out_len = scan_pos - pos
      mut out = Builder(out_len + 1)
      mut i = pos
      while i < scan_pos {
         out = builder_append_byte(out, load8(s, i))
         i += 1
      }
      def result = builder_to_str(out)
      builder_free(out)
      _json_set_pos(st, scan_pos + 1)
      return result
   }
   mut out = Builder(scan_pos - pos + 1)
   mut int cur = pos
   while cur < scan_pos {
      def c = load8(s, cur)
      if c == 92 {
         cur += 1
         if cur >= scan_pos {
            _json_set_error(st, "unterminated escape sequence")
            return ""
         }
         def esc = load8(s, cur)
         if esc == 34 {
            out = builder_append_byte(out, 34)
         } elif esc == 92 {
            out = builder_append_byte(out, 92)
         } elif esc == 47 {
            out = builder_append_byte(out, 47)
         } elif esc == 98 {
            out = builder_append_byte(out, 8)
         } elif esc == 102 {
            out = builder_append_byte(out, 12)
         } elif esc == 110 {
            out = builder_append_byte(out, 10)
         } elif esc == 114 {
            out = builder_append_byte(out, 13)
         } elif esc == 116 {
            out = builder_append_byte(out, 9)
         } elif esc == 117 {
            if cur + 4 >= scan_pos {
               _json_set_error(st, "invalid unicode escape")
               return ""
            }
            mut int cp1 = _json_hex4(s, cur + 1)
            if cp1 < 0 {
               _json_set_error(st, "invalid unicode escape")
               return ""
            }
            cur = cur + 4
            mut int cp = cp1
            if cp1 >= 55296 && cp1 <= 56319 {
               if cur + 6 >= scan_pos {
                  _json_set_error(st, "invalid unicode surrogate pair")
                  return ""
               }
               if load8(s, cur + 1) != 92 || load8(s, cur + 2) != 117 {
                  _json_set_error(st, "invalid unicode surrogate pair")
                  return ""
               }
               def cp2 = _json_hex4(s, cur + 3)
               if cp2 < 56320 || cp2 > 57343 {
                  _json_set_error(st, "invalid unicode surrogate pair")
                  return ""
               }
               cp = 65536 + ((cp1 - 55296) * 1024) + (cp2 - 56320)
               cur = cur + 6
            }
            if cp1 >= 56320 && cp1 <= 57343 {
               _json_set_error(st, "invalid unicode surrogate pair")
               return ""
            }
            if cp == 0 {
               out = builder_append_byte(out, 0)
            } elif cp < 128 {
               out = builder_append_byte(out, cp)
            } elif cp < 2048 {
               out = builder_append_byte(out, 192 | (cp >> 6))
               out = builder_append_byte(out, 128 | (cp & 63))
            } elif cp < 65536 {
               out = builder_append_byte(out, 224 | (cp >> 12))
               out = builder_append_byte(out, 128 | ((cp >> 6) & 63))
               out = builder_append_byte(out, 128 | (cp & 63))
            } else {
               out = builder_append_byte(out, 240 | (cp >> 18))
               out = builder_append_byte(out, 128 | ((cp >> 12) & 63))
               out = builder_append_byte(out, 128 | ((cp >> 6) & 63))
               out = builder_append_byte(out, 128 | (cp & 63))
            }
         } else {
            _json_set_error(st, "invalid escape sequence")
            return ""
         }
      } else {
         if c >= 0 && c < 32 {
            _json_set_error(st, "invalid control character in string")
            return ""
         }
         out = builder_append_byte(out, c)
      }
      cur += 1
   }
   def str result = builder_to_str(out)
   builder_free(out)
   _json_set_pos(st, scan_pos + 1)
   result
}

fn _json_parse_num(list st) any {
   mut int pos = _json_pos(st)
   def s = st.get(0)
   def int n = _json_size(st)
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
      _json_set_pos(st, pos)
      return neg ? (0 - int_val) : int_val
   }
   def str text = str.str_slice(s, start, pos)
   def f64 res = _json_parse_float_text(text)
   _json_set_pos(st, pos)
   return res
}

fn _json_try_decode(any s, int max_depth) dict {
   if !is_str(s) {
      _json_error = "json input must be a string"
      return _json_make_result(false, 0, _json_error, 0)
   }
   ;; Keep the input in a string-typed local before crossing the runtime ABI;
   ;; on Legacy LLVM a raw `any` can be passed as the boxed handle here.
   def str input = s
   def st = [input, 0, 0, input.len, 0, "", max_depth]
   def val = _json_parse_val(st, 0)
   _json_skip_ws(st)
   mut err = st.get(5, "")
   if err.len == 0 && _json_pos(st) != _json_size(st) {
      err = "trailing characters after JSON value"
      __store_item_fast(st, 5, err)
   }
   _json_error = st.get(5, "")
   if _json_error.len == 0 { return _json_make_result(true, val, "", _json_pos(st)) }
   _json_make_result(false, 0, _json_error, _json_pos(st))
}

fn json_try_decode(any s) dict {
   "Decodes JSON and returns `{ok, value, error, pos}`."
   _json_try_decode(s, 0)
}

fn json_try_decode_bounded(any s, int max_depth) dict {
   "Decodes JSON while rejecting container nesting beyond `max_depth`."
   if max_depth <= 0 {
      return _json_make_result(false, nil,
         "maximum JSON depth must be positive", 0)
   }
   _json_try_decode(s, max_depth)
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
   if is_nil(obj) { return "null" }
   if is_int(obj) { return to_str(obj) }
   if f.is_float(obj) { return __f64_to_json_str(obj) }
   if is_str(obj) { return _json_escape_string(obj) }
   if is_list(obj) || is_tuple(obj) || is_set(obj) { return _json_encode_seq(obj) }
   if is_dict(obj) {
      ; Enumerate through the dictionary bridge: `dict_items` returns a typed
      ; list whose Legacy LLVM return ABI can be mistaken for a string.
      def keys = dict_keys(obj)
      def n = dict_len(obj)
      mut i = 0
      mut out = Builder(max(16, n * 12 + 8))
      out = builder_append(out, "{")
      while i < n {
         def k = keys.get(i)
         def v = dict_get(obj, k, 0)
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
   print("✓ core.math.parse.data.json self-test passed")
}
