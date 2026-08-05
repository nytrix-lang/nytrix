;; Keywords: decompiler text expressions delimiters parsing
;; Balanced-delimiter primitives used by safe pseudocode expression cleanup.
module std.os.rev.decomp.text *

use std.core
use std.core.str as str

fn _clean_outer_balanced_parens_wrap(str expr0) bool {
   def expr = str.strip(expr0)
   if expr.len <= 2 || load8(expr, 0) != 40 || load8(expr, expr.len - 1) != 41 { return false }
   mut depth = 0
   mut quote = 0
   mut i = 0
   while i < expr.len {
      def ch = load8(expr, i)
      if quote != 0 {
         if ch == 92 && i + 1 < expr.len { i += 2 continue }
         if ch == quote { quote = 0 }
         i += 1
         continue
      }
      if ch == 34 || ch == 39 {
         quote = ch
         i += 1
         continue
      }
      if ch == 40 { depth += 1 }
      elif ch == 41 {
         depth -= 1
         if depth < 0 { return false }
         if depth == 0 && i < expr.len - 1 { return false }
      }
      i += 1
   }
   depth == 0
}

fn _clean_balanced_delimiters(str expr0) bool {
   def expr = str.strip(expr0)
   mut parens = 0
   mut brackets = 0
   mut quote = 0
   mut i = 0
   while i < expr.len {
      def ch = load8(expr, i)
      if quote != 0 {
         if ch == 92 && i + 1 < expr.len {
            i += 2
            continue
         }
         if ch == quote { quote = 0 }
         i += 1
         continue
      }
      if ch == 34 || ch == 39 {
         quote = ch
      } elif ch == 40 {
         parens += 1
      } elif ch == 41 {
         parens -= 1
         if parens < 0 { return false }
      } elif ch == 91 {
         brackets += 1
      } elif ch == 93 {
         brackets -= 1
         if brackets < 0 { return false }
      }
      i += 1
   }
   quote == 0 && parens == 0 && brackets == 0
}

fn _clean_strip_outer_balanced_parens(str expr0) str {
   mut expr = str.strip(expr0)
   mut guard = 0
   while guard < 8 && _clean_outer_balanced_parens_wrap(expr) {
      expr = str.strip(slice(expr, 1, expr.len - 1, 1))
      guard += 1
   }
   expr
}

fn _clean_strip_outer_parens(str expr) str {
   def clean = str.strip(expr)
   if clean.len > 1 && load8(clean, 0) == 40 && load8(clean, clean.len - 1) == 41 {
      return str.strip(slice(clean, 1, clean.len - 1, 1))
   }
   clean
}

fn _clean_top_level_find(str expr, str needle) int {
   if needle.len == 0 || expr.len < needle.len { return -1 }
   mut depth = 0
   mut quote = 0
   mut i = 0
   while i + needle.len <= expr.len {
      def ch = load8(expr, i)
      if quote != 0 {
         if ch == 92 && i + 1 < expr.len { i += 2 continue }
         if ch == quote { quote = 0 }
         i += 1
         continue
      }
      if ch == 34 || ch == 39 {
         quote = ch
         i += 1
         continue
      }
      if depth == 0 && slice(expr, i, i + needle.len, 1) == needle { return i }
      if ch == 40 { depth += 1 }
      elif ch == 41 && depth > 0 { depth -= 1 }
      elif ch == 91 { depth += 1 }
      elif ch == 93 && depth > 0 { depth -= 1 }
      i += 1
   }
   -1
}

fn _clean_top_level_split(str expr, str sep) list {
   mut out = []
   if sep.len == 0 { return [expr] }
   mut depth = 0
   mut quote = 0
   mut start = 0
   mut i = 0
   while i + sep.len <= expr.len {
      def ch = load8(expr, i)
      if quote != 0 {
         if ch == 92 && i + 1 < expr.len { i += 2 continue }
         if ch == quote { quote = 0 }
         i += 1
         continue
      }
      if ch == 34 || ch == 39 {
         quote = ch
         i += 1
         continue
      }
      if depth == 0 && slice(expr, i, i + sep.len, 1) == sep {
         out = out.append(str.strip(slice(expr, start, i, 1)))
         i += sep.len
         start = i
         continue
      }
      if ch == 40 { depth += 1 }
      elif ch == 41 && depth > 0 { depth -= 1 }
      elif ch == 91 { depth += 1 }
      elif ch == 93 && depth > 0 { depth -= 1 }
      i += 1
   }
   if start == 0 { return [expr] }
   out = out.append(str.strip(slice(expr, start, expr.len, 1)))
   out
}

fn _clean_literal_int_value(str raw) dict {
   if raw.len > 80 { return {"ok": false} }
   mut s = str.strip(_clean_strip_outer_balanced_parens(raw))
   if s.len == 0 { return {"ok": false} }
   mut sign = 1
   if str.startswith(s, "-") {
      sign = -1
      s = str.strip(slice(s, 1, s.len, 1))
   } elif str.startswith(s, "+") {
      s = str.strip(slice(s, 1, s.len, 1))
   }
   if s.len == 0 { return {"ok": false} }
   if str.startswith(s, "0x") || str.startswith(s, "0X") {
      if s.len <= 2 { return {"ok": false} }
      mut i = 2
      while i < s.len {
         def c = load8(s, i)
         if !((c >= 48 && c <= 57) || (c >= 65 && c <= 70) || (c >= 97 && c <= 102)) { return {"ok": false} }
         i += 1
      }
      return {"ok": true, "value": sign * str.parse_int(slice(s, 2, s.len, 1), 16)}
   }
   mut i = 0
   while i < s.len {
      if !str.ascii_is_digit(load8(s, i)) { return {"ok": false} }
      i += 1
   }
   {"ok": true, "value": sign * str.parse_int(s, 10)}
}

#main {
   assert(_clean_strip_outer_balanced_parens("((item))") == "item", "outer parentheses")
   assert(_clean_strip_outer_parens("(item)") == "item", "single outer parentheses")
   assert(_clean_balanced_delimiters("fn(\"[ok]\")"), "quoted delimiters")
   assert(!_clean_balanced_delimiters("fn([)"), "unbalanced delimiters")
   assert(_clean_top_level_find("call(a + b) + c", " + ") == 11, "top-level find")
   assert(_clean_top_level_split("a, call(b, c), d", ",").len == 3, "top-level split")
   assert(_clean_literal_int_value("(-0x10)").get("value", 0) == -16, "integer literal")
   print("✓ std.os.rev.decomp.text self-test passed")
}
