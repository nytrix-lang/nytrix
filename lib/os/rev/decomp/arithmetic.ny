;; Keywords: decompiler arithmetic expressions mba simplification
;; Arithmetic and mixed-boolean-arithmetic normalization for Ny pseudocode.
module std.os.rev.decomp.arithmetic *

use std.core
use std.core.str as str
use std.os.rev.decomp.text (_clean_outer_balanced_parens_wrap, _clean_strip_outer_balanced_parens, _clean_strip_outer_parens, _clean_top_level_find, _clean_literal_int_value)

fn _ny_infix_operator(str sym) str { sym == "^" ? "^^" : sym }

fn _clean_paren(str expr) str {
   def clean = str.strip(expr)
   if clean.len == 0 || str.find(clean, " ") < 0 || _clean_outer_balanced_parens_wrap(clean) { return clean }
   "(" + clean + ")"
}

fn _clean_literal_zero(str expr) bool {
   def clean = str.strip(expr)
   clean == "0" || clean == "0x0"
}

fn _clean_literal_one(str expr) bool {
   def clean = str.strip(expr)
   clean == "1" || clean == "0x1"
}

fn _clean_same_expr_text(str a0, str b0) bool { _clean_strip_outer_parens(a0) == _clean_strip_outer_parens(b0) }

fn _clean_const_mul_expr(str expr0) dict {
   def expr = _clean_strip_outer_parens(expr0)
   def p = _clean_top_level_find(expr, " * ")
   if p < 0 { return dict() }
   def left = str.strip(slice(expr, 0, p, 1))
   def right = str.strip(slice(expr, p + 3, expr.len, 1))
   def left_value = _clean_literal_int_value(left)
   if left_value.get("ok", false) { return {"factor": int(left_value.get("value", 0)), "value": _clean_strip_outer_parens(right)} }
   def right_value = _clean_literal_int_value(right)
   if right_value.get("ok", false) { return {"factor": int(right_value.get("value", 0)), "value": _clean_strip_outer_parens(left)} }
   dict()
}

fn _clean_div_expr_parts(str expr0) dict {
   def expr = _clean_strip_outer_parens(expr0)
   def p = _clean_top_level_find(expr, " / ")
   if p < 0 { return dict() }
   def base = _clean_strip_outer_parens(slice(expr, 0, p, 1))
   def raw_divisor = _clean_literal_int_value(slice(expr, p + 3, expr.len, 1))
   if !raw_divisor.get("ok", false) { return dict() }
   def divisor = int(raw_divisor.get("value", 0))
   if base.len == 0 || divisor <= 1 { return dict() }
   {"base": base, "divisor": divisor}
}

fn _clean_const_mul_text(int factor, str value0) str {
   def value = _clean_strip_outer_parens(value0)
   if factor == 0 { return "0" }
   if factor == 1 { return value }
   to_str(factor) + " * " + _clean_paren(value)
}

fn _clean_add_scaled_expr(str left0, str right0) str {
   def left = _clean_strip_outer_parens(left0)
   def right = _clean_strip_outer_parens(right0)
   if _clean_same_expr_text(left, right) { return _clean_const_mul_text(2, left) }
   def left_mul = _clean_const_mul_expr(left)
   if left_mul.len > 0 && _clean_same_expr_text(left_mul.get("value", ""), right) { return _clean_const_mul_text(int(left_mul.get("factor", 0)) + 1, right) }
   def right_mul = _clean_const_mul_expr(right)
   if right_mul.len > 0 && _clean_same_expr_text(right_mul.get("value", ""), left) { return _clean_const_mul_text(int(right_mul.get("factor", 0)) + 1, left) }
   if left_mul.len > 0 && right_mul.len > 0 && _clean_same_expr_text(left_mul.get("value", ""), right_mul.get("value", "")) {
      return _clean_const_mul_text(int(left_mul.get("factor", 0)) + int(right_mul.get("factor", 0)), left_mul.get("value", ""))
   }
   ""
}

fn _clean_sub_mod_expr(str left0, str right0) str {
   def left = _clean_strip_outer_parens(left0)
   def right_mul = _clean_const_mul_expr(right0)
   if right_mul.len == 0 { return "" }
   def quotient = _clean_div_expr_parts(right_mul.get("value", ""))
   if quotient.len == 0 { return "" }
   def divisor = int(quotient.get("divisor", 0))
   if divisor <= 1 || int(right_mul.get("factor", 0)) != divisor || !_clean_same_expr_text(left, quotient.get("base", "")) { return "" }
   left + " % " + to_str(divisor)
}

fn _clean_mba_same_expr(str a0, str b0) bool { _clean_strip_outer_balanced_parens(a0) == _clean_strip_outer_balanced_parens(b0) }

fn _clean_mba_pair_expr(str expr0, str sym) dict {
   def expr = _clean_strip_outer_balanced_parens(expr0)
   def op = " " + sym + " "
   def p = _clean_top_level_find(expr, op)
   if p < 0 { return dict() }
   def left = _clean_strip_outer_balanced_parens(slice(expr, 0, p, 1))
   def right = _clean_strip_outer_balanced_parens(slice(expr, p + op.len, expr.len, 1))
   if left.len == 0 || right.len == 0 { return dict() }
   {"left": left, "right": right, "op": sym}
}

fn _clean_mba_const_mul_expr(str expr0) dict {
   def expr = _clean_strip_outer_balanced_parens(expr0)
   def p = _clean_top_level_find(expr, " * ")
   if p < 0 { return dict() }
   def left = _clean_strip_outer_balanced_parens(slice(expr, 0, p, 1))
   def right = _clean_strip_outer_balanced_parens(slice(expr, p + 3, expr.len, 1))
   def left_value = _clean_literal_int_value(left)
   if left_value.get("ok", false) { return {"factor": int(left_value.get("value", 0)), "value": right} }
   def right_value = _clean_literal_int_value(right)
   if right_value.get("ok", false) { return {"factor": int(right_value.get("value", 0)), "value": left} }
   dict()
}

fn _clean_mba_scaled_pair_expr(str expr0, str sym, int factor) dict {
   def mul = _clean_mba_const_mul_expr(expr0)
   if mul.len == 0 || int(mul.get("factor", 0)) != factor { return dict() }
   _clean_mba_pair_expr(mul.get("value", ""), sym)
}

fn _clean_mba_pair_same_unordered(dict a, dict b) bool {
   if a.len == 0 || b.len == 0 { return false }
   (_clean_mba_same_expr(a.get("left", ""), b.get("left", "")) && _clean_mba_same_expr(a.get("right", ""), b.get("right", ""))) ||
   (_clean_mba_same_expr(a.get("left", ""), b.get("right", "")) && _clean_mba_same_expr(a.get("right", ""), b.get("left", "")))
}

fn _clean_mba_pair_sum(dict pair) str { _clean_paren(pair.get("left", "")) + " + " + _clean_paren(pair.get("right", "")) }
fn _clean_mba_pair_xor(dict pair) str { _clean_paren(pair.get("left", "")) + " ^^ " + _clean_paren(pair.get("right", "")) }
fn _clean_mba_xor_pair_expr(str expr0) dict { _clean_mba_pair_expr(expr0, "^^") }

fn _clean_mba_add_expr(str left0, str right0) str {
   def left_xor = _clean_mba_xor_pair_expr(left0)
   def right_and = _clean_mba_scaled_pair_expr(right0, "&", 2)
   if _clean_mba_pair_same_unordered(left_xor, right_and) { return _clean_mba_pair_sum(left_xor) }
   def left_and = _clean_mba_scaled_pair_expr(left0, "&", 2)
   def right_xor = _clean_mba_xor_pair_expr(right0)
   if _clean_mba_pair_same_unordered(left_and, right_xor) { return _clean_mba_pair_sum(right_xor) }
   def left_or = _clean_mba_pair_expr(left0, "|")
   def right_plain_and = _clean_mba_pair_expr(right0, "&")
   if _clean_mba_pair_same_unordered(left_or, right_plain_and) { return _clean_mba_pair_sum(left_or) }
   def left_plain_and = _clean_mba_pair_expr(left0, "&")
   def right_or = _clean_mba_pair_expr(right0, "|")
   if _clean_mba_pair_same_unordered(left_plain_and, right_or) { return _clean_mba_pair_sum(right_or) }
   ""
}

fn _clean_mba_sub_expr(str left0, str right0) str {
   def left_or = _clean_mba_pair_expr(left0, "|")
   def right_and = _clean_mba_pair_expr(right0, "&")
   if _clean_mba_pair_same_unordered(left_or, right_and) { return _clean_mba_pair_xor(left_or) }
   def left_plus = _clean_mba_pair_expr(left0, "+")
   def right_scaled_and = _clean_mba_scaled_pair_expr(right0, "&", 2)
   if _clean_mba_pair_same_unordered(left_plus, right_scaled_and) { return _clean_mba_pair_xor(left_plus) }
   ""
}

fn _clean_mba_expr_once(str expr0) str {
   def expr = _clean_strip_outer_balanced_parens(expr0)
   def plus = _clean_top_level_find(expr, " + ")
   if plus > 0 {
      def add = _clean_mba_add_expr(slice(expr, 0, plus, 1), slice(expr, plus + 3, expr.len, 1))
      if add.len > 0 { return add }
   }
   def minus = _clean_top_level_find(expr, " - ")
   if minus > 0 {
      def sub = _clean_mba_sub_expr(slice(expr, 0, minus, 1), slice(expr, minus + 3, expr.len, 1))
      if sub.len > 0 { return sub }
   }
   ""
}

fn _clean_simplify_mba_expr(str expr0) str {
   def raw = str.strip(expr0)
   def expr = _clean_strip_outer_balanced_parens(raw)
   def comparisons = [" == ", " != ", " <= ", " >= ", " < ", " > "]
   mut i = 0
   while i < comparisons.len {
      def comparison = comparisons[i]
      def p = _clean_top_level_find(expr, comparison)
      if p > 0 {
         def raw_left = str.strip(slice(expr, 0, p, 1))
         def raw_right = str.strip(slice(expr, p + comparison.len, expr.len, 1))
         def left = _clean_simplify_mba_expr(raw_left)
         def right = _clean_simplify_mba_expr(raw_right)
         if left != raw_left || right != raw_right { return left + comparison + right }
         return raw
      }
      i += 1
   }
   def direct = _clean_mba_expr_once(expr)
   direct.len > 0 ? direct : raw
}

fn _clean_binary_expr(str left0, str sym, str right0) str {
   def left = str.strip(left0)
   def right = str.strip(right0)
   if sym == "+" {
      if _clean_literal_zero(left) { return right }
      if _clean_literal_zero(right) { return left }
      def mba_add = _clean_mba_add_expr(left, right)
      if mba_add.len > 0 { return mba_add }
      def scaled = _clean_add_scaled_expr(left, right)
      if scaled.len > 0 { return scaled }
   }
   if sym == "-" {
      if _clean_literal_zero(right) { return left }
      def mba_sub = _clean_mba_sub_expr(left, right)
      if mba_sub.len > 0 { return mba_sub }
      def modded = _clean_sub_mod_expr(left, right)
      if modded.len > 0 { return modded }
   }
   if sym == "*" {
      if _clean_literal_one(left) { return right }
      if _clean_literal_one(right) { return left }
      if _clean_literal_zero(left) || _clean_literal_zero(right) { return "0" }
   }
   _clean_paren(left) + " " + _ny_infix_operator(sym) + " " + _clean_paren(right)
}

#main {
   assert(_clean_binary_expr("value", "+", "value") == "2 * value", "scaled addition")
   assert(_clean_simplify_mba_expr("(a ^^ b) + 2 * (a & b)") == "a + b", "mba addition")
   assert(_clean_binary_expr("value", "*", "1") == "value", "identity multiplication")
   print("✓ std.os.rev.decomp.arithmetic self-test passed")
}
