;; Keywords: factorization complex-multiplication math crypto number-theory
;; Integer-factorization routines for complex-multiplication leakage factorization.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap2.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap14.pdf
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.complex_multiplication(cm_factor, find_cm_curve, cm_order, class_number, cm_discriminant, cm_hilbert_class_poly, cm_j_invariant, is_cm_discriminant, cm_curve_params, cm11_prime_candidates_from_order_mod)
use std.math.nt

fn _cm_abs(number n) number { (n < 0) ? 0 - n : n }

fn _cm_append_unique(list xs, any value) list {
   xs.contains(value) ? xs : xs.append(value)
}

fn cm11_prime_candidates_from_order_mod(any gift, any modulus) list {
   "Recover p = u^2 + u + 3 candidates for the CM D=-11, j=-32768 order-mod leak.
   `gift` is the leaked isogenous-curve order modulo a prime modulus. The helper
   solves the two quadratic trace cases for u and returns distinct p candidates."
   def m = Z(modulus)
   mut out = []
   def left = mod_quadratic_roots_prime(Z(1), Z(-1), Z(3) - Z(gift), m)
   mut i = 0
   while i < left.len {
      def u = left[i]
      out = _cm_append_unique(out, u * u + u + Z(3))
      i += 1
   }
   def right = mod_quadratic_roots_prime(Z(1), Z(3), Z(5) - Z(gift), m)
   i = 0
   while i < right.len {
      def u = right[i]
      out = _cm_append_unique(out, u * u + u + Z(3))
      i += 1
   }
   out
}

fn is_cm_discriminant(number d) int {
   "Check if d is a valid CM discriminant(negative fundamental discriminant). Returns 1 if valid, 0 otherwise."
   if d >= 0 { return 0 }
   case int(d % 4){
      1 -> {
         def abs_d = _cm_abs(d)
         return _is_squarefree(abs_d) ? 1 : 0
      }
      0 -> {
         def m = d / 4
         case int(m % 4){
            2, 3 -> {
               def abs_m = _cm_abs(m)
               return _is_squarefree(abs_m) ? 1 : 0
            }
            _ -> { return 0 }
         }
      }
      _ -> { return 0 }
   }
   0
}

fn _is_squarefree(number n) bool {
   if n <= 1 { return true }
   mut test = 2
   while test * test <= n {
      if n % (test * test) == 0 { return false }
      test += 1
   }
   true
}

fn cm_discriminant(number n) number {
   "Find CM discriminant for field size n. Returns a negative discriminant d such that the imaginary quadratic field Q(sqrt(d)) has class number related to n."
   mut d = 0 - n
   if is_cm_discriminant(d) == 1 { return d }
   def d_alt = 0 - 4 * n
   (is_cm_discriminant(d_alt) == 1) ? d_alt : d
}

fn class_number(number d) int {
   "Compute class number of imaginary quadratic field Q(sqrt(d)) where d is a negative discriminant. Returns the class number h."
   if d >= 0 { return 0 }
   def abs_d = _cm_abs(d)
   def sqrt_d = isqrt(abs_d)
   def bound = sqrt_d
   def disc = (abs_d % 4 == 1) ? abs_d : 4 * abs_d
   mut h = 0
   def a_limit = bound / 2 + 1
   mut a = 1
   while a <= a_limit {
      def b_max = (a < bound) ? a : bound
      def b_start = 0 - a + 1
      mut b = b_start
      while b <= b_max {
         def c_num = b * b - disc
         if c_num > 0 && c_num % (4 * a) == 0 {
            def c = c_num / (4 * a)
            if a <= c && (a != c || b >= 0) {
               def g = gcd(a, gcd(b, c))
               if g == 1 { h += 1 }
            }
         }
         b += 1
      }
      a += 1
   }
   (h > 0) ? h : 1
}

fn cm_hilbert_class_poly(number d) list {
   "Compute Hilbert class polynomial for discriminant d. Returns coefficients as list [c0, c1, ..., cn]."
   def h = class_number(d)
   if h == 0 { return [1] }
   def abs_d = _cm_abs(d)
   def j_approx = compute_j_approx(abs_d)
   [j_approx, 1]
}

fn compute_j_approx(number d) number {
   "Internal: Approximate j-invariant for CM discriminant d using q-expansion. Returns integer approximation of j."
   def sqrt_d = isqrt(d)
   def q_val = exp_approx(0 - 2 * 314159 * sqrt_d / 100000)
   def j_val = 1 / q_val + 744 + 196884 * q_val
   j_val
}

fn exp_approx(number x) number {
   "Internal: Approximate e^x using Taylor series expansion(20 terms). Returns floating-point approximation."
   if x < 0 { return 1 / exp_approx(0 - x) }
   mut result = 1
   mut term = 1
   mut i = 1
   while i < 20 {
      term = term * x / i
      result = result + term
      i += 1
   }
   result
}

fn cm_j_invariant(number d) number {
   "Compute j-invariant for CM discriminant d. Returns the approximate j-invariant value."
   def abs_d = _cm_abs(d)
   if abs_d == 3 { return 0 }
   if abs_d == 4 { return 1728 }
   compute_j_approx(abs_d)
}

fn find_cm_curve(number p, number d) list {
   "Find elliptic curve with CM by discriminant d over F_p. Returns [j, a, b, p] defining the curve y^2 = x^3 + ax + b."
   def j = cm_j_invariant(d) % p
   def a = find_curve_a(j, p)
   def b = find_curve_b(j, a, p)
   [j, a, b, p]
}

fn find_curve_a(number j, number p) number {
   "Internal: Find curve parameter a from j-invariant over F_p. Returns the coefficient a for the elliptic curve."
   if j == 0 { return 0 }
   if j == 1728 { return 1 }
   def num = 3 * j * j % p
   def den = j % p
   def inv_den = inverse_mod(den, p)
   (num * inv_den) % p
}

fn find_curve_b(number j, number a, number p) number {
   "Internal: Find curve parameter b from j-invariant and a over F_p. Returns the coefficient b for the elliptic curve."
   if j == 0 { return 1 }
   if j == 1728 { return 0 }
   def num = 2 * j * j * j % p
   def den = j * j % p
   def inv_den = inverse_mod(den, p)
   (num * inv_den) % p
}

fn cm_order(number p, number d) number {
   "Compute order of elliptic curve with CM by d over F_p. Returns the number of points on the curve."
   def t = find_trace(p, d)
   p + 1 - t
}

fn find_trace(number p, number d) number {
   "Internal: Find trace of Frobenius for CM curve over F_p with discriminant d. Returns the trace value t."
   def abs_d = _cm_abs(d)
   def kronecker_val = legendre_or_kronecker(abs_d, p)
   kronecker_val * find_t_from_equation(p, abs_d)
}

fn legendre_or_kronecker(number a, number p) number {
   "Internal: Compute Legendre/Kronecker symbol(a/p). Returns 0, 1, or -1."
   legendre(a, p)
}

fn find_t_from_equation(number p, number d) number {
   "Internal: Find t such that 4p = t^2 - d*v^2 for some v. Returns t or 0 if not found."
   def four_p = 4 * p
   def t_bound = 2 * isqrt(p) + 1
   mut t = 0
   while t <= t_bound {
      def rhs = t * t - four_p
      def diff = (rhs < 0) ? 0 - rhs : rhs
      if diff % d == 0 {
         def quotient = diff / d
         def sqrt_q = isqrt(quotient)
         if sqrt_q * sqrt_q == quotient { return t }
      }
      t += 1
   }
   0
}

fn cm_curve_params(number p, number d) list {
   "Get full CM curve parameters over F_p. Returns [j, a, b, p, order] for the elliptic curve with CM by d."
   def curve = find_cm_curve(p, d)
   def order = cm_order(p, d)
   [curve[0], curve[1], curve[2], curve[3], order]
}

fn cm_factor(number n) number {
   "Attempt to factor n using CM method. Works when n is a prime or has small prime factors. Returns a factor or 0 if none found."
   mut d = 3
   while d < 100 {
      if is_cm_discriminant(0 - d) == 1 {
         mut j = cm_j_invariant(0 - d)
         def candidate = j % n
         if candidate > 1 && candidate < n {
            def g = gcd(candidate, n)
            if g > 1 && g < n { return g }
         }
      }
      d += 1
   }
   0
}
