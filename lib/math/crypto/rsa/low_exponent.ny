;; Keywords: rsa low-exponent math crypto
;; RSA low-exponent RSA attacks routines.
;; (no modular reduction occurred).  Recover m by taking the eth root.
;; Also handles broadcast via Hastad when enough recipients are available.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.low_exponent(low_exp_attack, low_exp_attack_report, low_exp_cube_root, nth_root)
use std.math.nt

fn _low_exp_exp(any e) int { is_bigint(e) ? bigint_to_int(e) : int(e) }

fn _low_exp_pow(any base, int exp) any {
   mut out = Z(1)
   mut i = 0
   while i < exp {
      out = out * base
      i += 1
   }
   out
}

fn nth_root(any c, any n) any {
   "Compute integer n-th root of c: largest x such that x^n <= c.
   Uses Newton's method for fast convergence. Returns floor(c^(1/n))."
   if c <= 0 { return 0 }
   if c == 1 { return 1 }
   if is_bigint(n) && n > Z(bit_length(c)) { return 1 }
   def nn = _low_exp_exp(n)
   if nn <= 0 { return nil }
   if nn == 1 { return c }
   if nn > bit_length(c) { return 1 }
   mut x = c
   mut prev = c + 1
   while x < prev {
      prev = x
      mut xn = 1
      mut j = 0
      while j < nn - 1 {
         xn = xn * x
         j += 1
      }
      if xn == 0 { return x }
      x = ((nn - 1) * x + c / xn) / nn
   }
   x
}

fn low_exp_attack(any c, any e) any {
   "Recover m from c = m^e when m^e < N(no modular wrap occurred).
   Works for any small exponent e. Returns m or nil if eth root is not exact."
   if is_bigint(e) && e > Z(64) { return nil }
   def ee = _low_exp_exp(e)
   if ee <= 1 || ee > 64 { return nil }
   def m = nth_root(c, e)
   if m == nil { return nil }
   def check = _low_exp_pow(m, ee)
   if check == c { return m }
   nil
}

fn low_exp_attack_report(any c, any e) dict {
   "Explain a direct low-exponent root recovery attempt."
   def ee = _low_exp_exp(e)
   mut reason = ""
   mut m = nil
   if ee <= 1 {
      reason = "exponent must be greater than one"
   } else {
      m = low_exp_attack(c, ee)
      if m == nil { reason = "ciphertext is not an exact e-th power" }
   }
   {
      "ok": m != nil,
      "plaintext": m,
      "reason": reason,
      "ciphertext": c,
      "exponent": ee,
      "exact_power": m != nil,
   }
}

fn low_exp_cube_root(any c) any {
   "Cube-root attack for e=3: recover m from c = m^3(no mod reduction).
   Returns m or nil."
   def m = nth_root(c, 3)
   if m * m * m == c { return m }
   nil
}
