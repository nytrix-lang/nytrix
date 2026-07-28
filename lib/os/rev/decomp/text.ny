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

#main {
   assert(_clean_strip_outer_balanced_parens("((item))") == "item", "outer parentheses")
   assert(_clean_balanced_delimiters("fn(\"[ok]\")"), "quoted delimiters")
   assert(!_clean_balanced_delimiters("fn([)"), "unbalanced delimiters")
   print("✓ std.os.rev.decomp.text self-test passed")
}
