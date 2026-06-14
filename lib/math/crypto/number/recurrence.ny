;; Keywords: number-theory recurrence math crypto
;; Crypto number-theory routines for linear recurrence solving modulo integers.
;; References:
;; - std.math.crypto.number
;; - std.math.crypto
module std.math.crypto.number.recurrence(linear_recurrence_mod)
use std.math.integer (Z, mod)
use std.math.matrix (Matrix, matrix_pow_mod, _matrix_get)

fn linear_recurrence_mod(list coeffs, list initials, any n, any modulus) any {
   "Return F(n) mod modulus for F(i)=sum_j coeffs[j]*F(i-j-1)."
   def k = coeffs.len
   if k == 0 { panic("linear_recurrence_mod: empty coefficient list") }
   if initials.len != k { panic("linear_recurrence_mod: initials length mismatch") }
   def nn = Z(n)
   def mm = Z(modulus)
   if nn < Z(0) { panic("linear_recurrence_mod: negative index") }
   if mm <= Z(0) { panic("linear_recurrence_mod: modulus must be positive") }
   if nn < Z(k) { return mod(initials[int(nn)], mm) }
   mut rows = []
   mut first = []
   mut j = 0
   while j < k {
      first = first.append(mod(coeffs[j], mm))
      j += 1
   }
   rows = rows.append(first)
   mut i = 1
   while i < k {
      mut row = []
      j = 0
      while j < k {
         row = row.append(i == j + 1 ? Z(1) : Z(0))
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   def m = Matrix(rows)
   def mp = matrix_pow_mod(m, nn - Z(k - 1), mm)
   mut out = Z(0)
   j = 0
   while j < k {
      out = mod(out + _matrix_get(mp, 0, j) * initials[k - 1 - j], mm)
      j += 1
   }
   out
}
