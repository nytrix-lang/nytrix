;; Keywords: data serialization toml parse
;; Practical TOML parser/generator for flat and sectioned configuration files.
;; References:
;; - std.parse.data
;; - std.parse
module std.parse.data.toml(toml_decode, toml_try_decode, toml_last_error, toml_encode, decode, encode)
use std.core
use std.core.str as str
use std.parse.data.json as json

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
   mut i = 0
   while i < line.len {
      def c = load8(line, i)
      if quote != 0 {
         if c == quote { quote = 0 }
      } elif c == 34 || c == 39 {
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
      if (a == 34 && b == 34) || (a == 39 && b == 39) { return str.str_slice(v, 1, v.len - 1) }
   }
   v
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

fn _toml_literal(any lo) any {
   case lo {
      "true" -> true
      "false" -> false
      _ -> nil
   }
}

fn _toml_scalar(any raw) any {
   mut v = str.strip(raw)
   if v.len == 0 { return "" }
   def lo = str.lower(v)
   def lit = _toml_literal(lo)
   if lit != nil { return lit }
   if load8(v, 0) == 91 && load8(v, v.len - 1) == 93 {
      def inner = str.str_slice(v, 1, v.len - 1)
      def parts = str.split(inner, ",")
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
   _toml_unquote(v)
}

fn _toml_section(dict root, any name) any {
   def parts = str.split(name, ".")
   mut cur = root
   mut i = 0
   while i < parts.len {
      def key = _toml_unquote(str.strip(parts.get(i)))
      if key.len == 0 { return 0 }
      mut next = cur.get(key, 0)
      if !is_dict(next) {
         next = dict(8)
         cur[key] = next
      }
      cur = next
      i += 1
   }
   cur
}

fn toml_try_decode(any src) dict {
   "Decodes TOML sections, key/value pairs, scalars, and simple arrays."
   _toml_error = ""
   if !is_str(src) { return _toml_result(false, 0, "toml input must be a string", 0) }
   def lines = str.split(src, "\n")
   mut root = dict(16)
   mut current = root
   mut i = 0
   while i < lines.len {
      def line_no = i + 1
      def line = str.strip(_toml_strip_comment(lines.get(i)))
      if line.len == 0 { i += 1 continue }
      if load8(line, 0) == 91 && load8(line, line.len - 1) == 93 {
         def sec = str.strip(str.str_slice(line, 1, line.len - 1))
         current = _toml_section(root, sec)
         if !is_dict(current) {
            _toml_set_error("invalid section name")
            return _toml_result(false, 0, _toml_error, line_no)
         }
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
      current[key] = val
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
