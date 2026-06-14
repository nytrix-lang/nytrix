;; Keywords: factorization quadratic-form math crypto number-theory
;; Integer-factorization routines for quadratic-form factorization.
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.quadratic_form(factor_square_offsets, factor_square_offsets_scan, square_offset_factor)
use std.core
use std.math.nt

fn factor_square_offsets_scan(any n, any p_offset, any q_offset, any scan) any {
   "Factor n when p=a^2+p_offset and q=b^2+q_offset by scanning ab near sqrt(n)."
   def nn = Z(n)
   def po = Z(p_offset)
   def qo = Z(q_offset)
   def sum_const = po * qo
   def denom = Z(2) * qo
   def root = isqrt(nn)
   mut x = root
   mut left = Z(scan)
   while left >= Z(0) {
      def e1 = nn - x * x - sum_const
      if e1 > Z(0) {
         def disc = e1 * e1 - Z(4) * po * qo * x * x
         if disc >= Z(0) {
            def r = isqrt(disc)
            if r * r == disc {
               def cands = [e1 + r, e1 - r]
               mut i = 0
               while i < cands.len {
                  def cand = cands.get(i)
                  if cand % denom == Z(0) {
                     def a2 = cand / denom
                     def p = a2 + po
                     if p > Z(1) && nn % p == Z(0) { return [p, nn / p] }
                  }
                  i += 1
               }
            }
         }
      }
      x -= Z(1)
      left -= Z(1)
   }
   nil
}

fn factor_square_offsets(any n, any p_offset, any q_offset) any {
   "Factor n when p=a^2+p_offset and q=b^2+q_offset, using the default scan window."
   factor_square_offsets_scan(n, p_offset, q_offset, 100000)
}

fn square_offset_factor(any n, any p_offset, any q_offset) any {
   "Alias for factor_square_offsets."
   factor_square_offsets(n, p_offset, q_offset)
}
