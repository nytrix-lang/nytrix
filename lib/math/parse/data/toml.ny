;; Keywords: data serialization toml parse
;; Practical TOML parser/generator for flat and sectioned configuration files.
;; References:
;; - core.math.parse.data
;; - core.math.parse
module core.math.parse.data.toml(toml_decode, toml_try_decode, toml_last_error, toml_encode, decode, encode)
use core
use core.str as str
use core.math.parse.data.json as json

mut _toml_error = ""

fn toml_last_error() str {
   "Returns the error from the last TOML decode attempt."
   _toml_error
}

fn _toml_result(any ok, any value, any error, any line) dict {
   {"ok": ok, "value": value, "error": error, "line": line}
}

fn _toml_set_error(any msg) int {
   if _toml_error.len == 0 { _toml_error = msg }
   0
}

fn _toml_strip_comment(str line) str {
   mut quote = 0
   mut escaped = false
   mut i = 0
   while i < line.len {
      def c = load8(line, i)
      if quote != 0 {
         if quote == 34 && escaped { escaped = false }
         elif quote == 34 && c == 92 { escaped = true }
         elif c == quote { quote = 0 }
         i += 1
         continue
      }
      if c == 34 || c == 39 {
         quote = c
      } elif c == 35 {
         return str.str_slice(line, 0, i)
      }
      i += 1
   }
   line
}

fn _toml_unquote(str v) str {
   if v.len >= 2 {
      def a, b = load8(v, 0), load8(v, v.len - 1)
      if (a == 34 && b == 34) || (a == 39 && b == 39) {
         def decoded = _toml_decode_quoted(v)
         if decoded.get("ok", false) { return decoded.get("value") }
         return ""
      }
   }
   v
}

fn _toml_hex_value(int byte) int {
   if byte >= 48 && byte <= 57 { return byte - 48 }
   if byte >= 65 && byte <= 70 { return byte - 55 }
   if byte >= 97 && byte <= 102 { return byte - 87 }
   -1
}

fn _toml_decode_quoted(str source) dict {
   if source.len < 2 {
      return {"ok":false, "value":nil, "error":"unterminated quoted string"}
   }
   def quote = load8(source, 0)
   if load8(source, source.len - 1) != quote ||
   (quote != 34 && quote != 39) {
      return {"ok":false, "value":nil, "error":"unterminated quoted string"}
   }
   if quote == 39 {
      def value = str.str_slice(source, 1, source.len - 1)
      if str.find(value, "'") >= 0 {
         return {"ok":false, "value":nil,
            "error":"single quote in single-line literal string"}
      }
      return {"ok":true, "value":value, "error":""}
   }
   mut out = Builder(source.len)
   mut i = 1
   while i < source.len - 1 {
      def byte = load8(source, i)
      if byte == 92 {
         i += 1
         if i >= source.len - 1 {
            builder_free(out)
            return {"ok":false, "value":nil, "error":"unterminated escape sequence"}
         }
         def escape = load8(source, i)
         if escape == 34 { out = builder_append_byte(out, 34) }
         elif escape == 92 { out = builder_append_byte(out, 92) }
         elif escape == 98 { out = builder_append_byte(out, 8) }
         elif escape == 116 { out = builder_append_byte(out, 9) }
         elif escape == 110 { out = builder_append_byte(out, 10) }
         elif escape == 102 { out = builder_append_byte(out, 12) }
         elif escape == 114 { out = builder_append_byte(out, 13) }
         elif escape == 117 || escape == 85 {
            def digits = escape == 117 ? 4 : 8
            if i + digits >= source.len - 1 {
               builder_free(out)
               return {"ok":false, "value":nil, "error":"invalid Unicode escape"}
            }
            mut codepoint = 0
            mut j = 1
            while j <= digits {
               def nibble = _toml_hex_value(load8(source, i + j))
               if nibble < 0 {
                  builder_free(out)
                  return {"ok":false, "value":nil, "error":"invalid Unicode escape"}
               }
               codepoint = codepoint * 16 + nibble
               j += 1
            }
            if codepoint == 0 || codepoint > 1114111 ||
            (codepoint >= 55296 && codepoint <= 57343) {
               builder_free(out)
               return {"ok":false, "value":nil, "error":"invalid Unicode scalar value"}
            }
            if codepoint < 128 {
               out = builder_append_byte(out, codepoint)
            } elif codepoint < 2048 {
               out = builder_append_byte(out, 192 | (codepoint >> 6))
               out = builder_append_byte(out, 128 | (codepoint & 63))
            } elif codepoint < 65536 {
               out = builder_append_byte(out, 224 | (codepoint >> 12))
               out = builder_append_byte(out, 128 | ((codepoint >> 6) & 63))
               out = builder_append_byte(out, 128 | (codepoint & 63))
            } else {
               out = builder_append_byte(out, 240 | (codepoint >> 18))
               out = builder_append_byte(out, 128 | ((codepoint >> 12) & 63))
               out = builder_append_byte(out, 128 | ((codepoint >> 6) & 63))
               out = builder_append_byte(out, 128 | (codepoint & 63))
            }
            i += digits
         } else {
            builder_free(out)
            return {"ok":false, "value":nil, "error":"invalid escape sequence"}
         }
      } else {
         if byte < 32 || byte == 127 {
            builder_free(out)
            return {"ok":false, "value":nil, "error":"unescaped control character"}
         }
         out = builder_append_byte(out, byte)
      }
      i += 1
   }
   def value = builder_to_str(out)
   builder_free(out)
   return {"ok":true, "value":value, "error":""}
}

fn _toml_numeric_like(str v) bool {
   if v.len == 0 { return false }
   mut i = 0
   if load8(v, 0) == 45 { i = 1 }
   if i >= v.len { return false }
   mut saw_digit = false
   mut dots = 0
   while i < v.len {
      def c = load8(v, i)
      if c >= 48 && c <= 57 { saw_digit = true } elif c == 46 {
         dots += 1
         if dots > 1 { return false }
      } else {
         return false
      }
      i += 1
   }
   saw_digit
}

fn _toml_scalar(any raw) any {
   mut v = str.strip(raw)
   if v.len == 0 { return "" }
   def lo = str.lower(v)
   if lo == "true" { return true }
   if lo == "false" { return false }
   if load8(v, 0) == 91 && load8(v, v.len - 1) == 93 {
      def inner = str.str_slice(v, 1, v.len - 1)
      def parts = _toml_split_array(inner)
      mut out = list(parts.len)
      mut i = 0
      while i < parts.len {
         def p = str.strip(parts.get(i))
         if p.len > 0 { out = out.append(_toml_scalar(p)) }
         i += 1
      }
      return out
   }
   if _toml_numeric_like(v) {
      if str.find(v, ".") >= 0 { return str.atof(v) }
      return str.atoi(v)
   }
   if v.len >= 2 && ((load8(v, 0) == 34 && load8(v, v.len - 1) == 34) ||
      (load8(v, 0) == 39 && load8(v, v.len - 1) == 39)) {
      def decoded = _toml_decode_quoted(v)
      if !decoded.get("ok", false) {
         return {"__toml_error":true, "message":decoded.get("error", "invalid string")}
      }
      return decoded.get("value")
   }
   _toml_unquote(v)
}

fn _toml_split_array(str inner) list {
   mut parts = []
   mut start = 0
   mut quote = 0
   mut escaped = false
   mut depth = 0
   mut i = 0
   while i < inner.len {
      def c = load8(inner, i)
      if quote == 34 {
         if escaped {
            escaped = false
         } else {
            if c == 92 {
               escaped = true
            } elif c == 34 {
               quote = 0
            }
         }
      } elif quote == 39 {
         if c == 39 { quote = 0 }
      } else {
         if c == 34 || c == 39 {
            quote = c
         } elif c == 91 {
            depth += 1
         } elif c == 93 && depth > 0 {
            depth -= 1
         } elif c == 44 && depth == 0 {
            parts = parts.append(str.str_slice(inner, start, i))
            start = i + 1
         }
      }
      i += 1
   }
   if start < inner.len { parts = parts.append(str.str_slice(inner, start, inner.len)) }
   parts
}

fn _toml_scalar_error(any value) str {
   if is_dict(value) && value.get("__toml_error", false) {
      return value.get("message", "invalid quoted string")
   }
   if is_list(value) {
      mut i = 0
      while i < value.len {
         def error = _toml_scalar_error(value.get(i))
         if error.len > 0 { return error }
         i += 1
      }
   }
   ""
}

fn _toml_section(dict root, any name) any {
   def parts = str.split(name, ".")
   mut current = root
   mut parents = []
   mut names = []
   mut i = 0
   while i < parts.len {
      def section_key = _toml_unquote(str.strip(parts.get(i)))
      if section_key.len == 0 { return 0 }
      mut next = current.get(section_key, 0)
      if !is_dict(next) { next = dict(8) }
      parents = parents.append(current)
      names = names.append(section_key)
      current = next
      i += 1
   }
   i = names.len - 1
   while i >= 0 {
      mut parent = parents.get(i)
      parent = dict_set(parent, names.get(i), current)
      current = parent
      i -= 1
   }
   current
}

fn _toml_store_path(dict root, list parts, int index, str key, any value) dict {
   mut names = []
   mut parents = []
   mut current = root
   mut i = index
   while i < parts.len {
      def name = _toml_unquote(str.strip(parts.get(i)))
      if name.len == 0 { return root }
      mut child = current.get(name, 0)
      if !is_dict(child) { child = dict(8) }
      parents = parents.append(current)
      names = names.append(name)
      current = child
      i += 1
   }
   current = dict_set(current, key, value)
   i = names.len - 1
   while i >= 0 {
      mut parent = parents.get(i)
      parent = dict_set(parent, names.get(i), current)
      current = parent
      i -= 1
   }
   current
}

fn toml_try_decode(any src) dict {
   "Decodes TOML sections, key/value pairs, scalars, and simple arrays."
   _toml_error = ""
   if !is_str(src) { return _toml_result(false, 0, "toml input must be a string", 0) }
   def lines = str.split(src, "\n")
   mut root = dict(16)
   mut section_name = ""
   mut i = 0
   while i < lines.len {
      def line_no = i + 1
      def line = str.strip(_toml_strip_comment(lines.get(i)))
      if line.len == 0 { i += 1 continue }
      if load8(line, 0) == 91 && load8(line, line.len - 1) == 93 {
         def sec = str.strip(str.str_slice(line, 1, line.len - 1))
         root = _toml_section(root, sec)
         if !is_dict(root) {
            _toml_set_error("invalid section name")
            return _toml_result(false, 0, _toml_error, line_no)
         }
         section_name = sec
         i += 1
         continue
      }
      def eq = str.find(line, "=")
      if eq < 0 {
         _toml_set_error("expected key = value")
         return _toml_result(false, 0, _toml_error, line_no)
      }
      def key = _toml_unquote(str.strip(str.str_slice(line, 0, eq)))
      if key.len == 0 {
         _toml_set_error("empty key")
         return _toml_result(false, 0, _toml_error, line_no)
      }
      def val = _toml_scalar(str.str_slice(line, eq + 1, line.len))
      def value_error = _toml_scalar_error(val)
      if value_error.len > 0 {
         _toml_set_error(value_error)
         return _toml_result(false, 0, _toml_error, line_no)
      }
      def section_parts = section_name.len > 0 ? str.split(section_name, ".") : []
      root = _toml_store_path(root, section_parts, 0, key, val)
      i += 1
   }
   _toml_result(true, root, "", 0)
}

fn toml_decode(any src) any {
   "Decodes TOML and returns 0 on error; inspect toml_last_error() for details."
   def res = toml_try_decode(src)
   _toml_error = res.get("error", "")
   if !res.get("ok", false) { return 0 }
   res.get("value")
}

fn _toml_encode_scalar(any v) str {
   if type(v) == "bool" {
      if v { return "true" }
      return "false"
   }
   if is_int(v) || is_float(v) { return to_str(v) }
   if v == 0 { return "\"\"" }
   if is_list(v) {
      mut out = Builder(32)
      out = builder_append(out, "[")
      mut i = 0
      while i < v.len {
         out = builder_append(out, _toml_encode_scalar(v.get(i)))
         if i + 1 < v.len { out = builder_append(out, ", ") }
         i += 1
      }
      out = builder_append(out, "]")
      def s = builder_to_str(out)
      builder_free(out)
      return s
   }
   json.json_encode(to_str(v))
}

fn _toml_encode_table(any out, str prefix, dict table) any {
   def items = dict_items(table)
   mut i = 0
   while i < items.len {
      def p = items.get(i)
      if !is_dict(p.get(1)) { out = builder_append(out, to_str(p.get(0)) + " = " + _toml_encode_scalar(p.get(1)) + "\n") }
      i += 1
   }
   i = 0
   while i < items.len {
      def p = items.get(i)
      def key = to_str(p.get(0))
      def val = p.get(1)
      if is_dict(val) {
         def name = prefix.len > 0 ? prefix + "." + key : key
         out = builder_append(out, "\n[" + name + "]\n")
         out = _toml_encode_table(out, name, val)
      }
      i += 1
   }
   out
}

fn toml_encode(any value) str {
   "Encodes a dict as TOML."
   if !is_dict(value) { return _toml_encode_scalar(value) + "\n" }
   mut out = Builder(128)
   out = _toml_encode_table(out, "", value)
   def s = builder_to_str(out)
   builder_free(out)
   s
}

fn decode(any src) any { toml_decode(src) }

fn encode(any value) str { toml_encode(value) }
