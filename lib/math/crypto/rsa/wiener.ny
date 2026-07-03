;; Keywords: rsa wiener math crypto
;; RSA Wiener small-private-exponent attack routines.
;; convergents of e/N.  Works when d < N^(1/4) / 3.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.wiener(wiener_attack, wiener_attack_entry, convergents, factorize_from_phi, wiener_lattice_attack, wiener_attack_lattice_entry, wiener_attack_common_prime, wiener_attack_common_prime_entry)
use std.math.nt
use std.math.crypto.factorization.known_phi
use std.math.crypto.lattice.lll
use std.math.crypto.rsa.common_prime
use std.math.crypto.rsa.op (compute_phi, compute_d)
use std.math.matrix

fn convergents(any num, any den) list {
   "Return list of [k, h, k2] convergent triples for num/den.
   Each triple is [quotient_sequence_index, numerator, denominator]."
   mut convs = list(0)
   mut a_num = Z(num)
   mut a_den = Z(den)
   mut h_prev2 = Z(0)
   mut h_prev1 = Z(1)
   mut k_prev2 = Z(1)
   mut k_prev1 = Z(0)
   mut idx = Z(0)
   while bigint_eq(a_den, Z(0)) == false {
      def a_q = a_num / a_den
      def a_rem = a_num % a_den
      def h = a_q * h_prev1 + h_prev2
      def k = a_q * k_prev1 + k_prev2
      convs = convs.append([idx, h, k])
      h_prev2 = h_prev1
      h_prev1 = h
      k_prev2 = k_prev1
      k_prev1 = k
      a_num = a_den
      a_den = a_rem
      idx = idx + Z(1)
   }
   convs
}

fn factorize_from_phi(any N, any phi) list {
   "Given N = p*q and phi(N), recover [p, q].
   We have p+q = N - phi + 1 and pq = N, so p,q are roots of
   x^2 - (p+q)x + N = 0."
   def sum_pq = N - phi + Z(1)
   def disc = sum_pq * sum_pq - Z(4) * N
   def s = isqrt(disc)
   def p = (sum_pq + s) / Z(2)
   def q = (sum_pq - s) / Z(2)
   [p, q]
}

fn _wiener_small_d_fallback(any N, any e, any limit=4096) any {
   "Bounded verifier for tiny private-exponent fixtures. The continued-fraction
   path above is the primary attack ; this keeps small known vectors strict even
   when callers pass mixed integer carriers."
   if !is_int(N) || !is_int(e) { return nil }
   def NN = N
   def ee = e
   def base = power_mod(2, ee, NN)
   mut d = 1
   while d <= limit {
      if power_mod(base, d, NN) == 2 {
         mut k = 1
         while k <= d + 1 {
            if ((ee * d - 1) % k) == 0 {
               def phi = (ee * d - 1) / k
               def facs = factor_from_phi(NN, phi)
               if facs != nil {
                  def p_cand = facs[0]
                  def q_cand = facs[1]
                  if p_cand * q_cand == NN {
                     def p_final = p_cand > q_cand ? p_cand : q_cand
                     def q_final = p_cand > q_cand ? q_cand : p_cand
                     return [d, p_final, q_final]
                  }
               }
            }
            k += 1
         }
      }
      d += 1
   }
   nil
}

fn _wiener_square_residue_filter(any n) bool {
   "Cheap prefilter before big isqrt: every square must be a quadratic
   residue modulo 64."
   case int(n & Z(63)){
      0, 1, 4, 9, 16, 17, 25, 33, 36, 41, 49, 57 -> true
      _ -> false
   }
}

fn _wiener_square_residue_filter_int(int n) bool {
   case n & 63 {
      0, 1, 4, 9, 16, 17, 25, 33, 36, 41, 49, 57 -> true
      _ -> false
   }
}

fn _wiener_attack_small_int(any N, any e) any {
   def NN_big = Z(N)
   if bit_length(NN_big) > 52 { return nil }
   def NN = int(NN_big)
   def ee = int(e)
   if NN <= 0 || ee <= 0 { return nil }
   mut a_num = ee
   mut a_den = NN
   mut h_prev2 = 0
   mut h_prev1 = 1
   mut k_prev2 = 1
   mut k_prev1 = 0
   while a_den != 0 {
      def a_q = a_num / a_den
      def a_rem = a_num % a_den
      def k = a_q * h_prev1 + h_prev2
      def d_cand = a_q * k_prev1 + k_prev2
      h_prev2 = h_prev1
      h_prev1 = k
      k_prev2 = k_prev1
      k_prev1 = d_cand
      a_num = a_den
      a_den = a_rem
      if k == 0 { continue }
      def ed_minus_one = ee * d_cand - 1
      if (ed_minus_one % k) != 0 { continue }
      def phi_cand = ed_minus_one / k
      def sum_pq = NN - phi_cand + 1
      def disc = sum_pq * sum_pq - 4 * NN
      if disc < 0 { continue }
      if !_wiener_square_residue_filter_int(disc) { continue }
      def s = int(isqrt(Z(disc)))
      if s * s != disc { continue }
      def p_cand = (sum_pq + s) / 2
      def q_cand = (sum_pq - s) / 2
      if p_cand * q_cand == NN {
         def p_final = p_cand > q_cand ? p_cand : q_cand
         def q_final = p_cand > q_cand ? q_cand : p_cand
         return [Z(d_cand), Z(p_final), Z(q_final)]
      }
   }
   nil
}

fn wiener_attack(any N, any e) any {
   "Recover [d, p, q] from RSA public key(N, e) when d is small.
   Returns [d, p, q] on success or nil on failure."
   def small_hit = _wiener_attack_small_int(N, e)
   if small_hit != nil { return small_hit }
   def NN = Z(N)
   def ee = Z(e)
   def zero = Z(0)
   def one = Z(1)
   def two = Z(2)
   def four = Z(4)
   mut a_num = ee
   mut a_den = NN
   mut h_prev2 = zero
   mut h_prev1 = one
   mut k_prev2 = one
   mut k_prev1 = zero
   while bigint_eq(a_den, zero) == false {
      def a_q = a_num / a_den
      def a_rem = a_num % a_den
      def k = a_q * h_prev1 + h_prev2
      def d_cand = a_q * k_prev1 + k_prev2
      h_prev2 = h_prev1
      h_prev1 = k
      k_prev2 = k_prev1
      k_prev1 = d_cand
      a_num = a_den
      a_den = a_rem
      if k == zero {
         continue
      }
      def ed_minus_one = ee * d_cand - one
      if (ed_minus_one % k) != zero {
         continue
      }
      def phi_cand = ed_minus_one / k
      def sum_pq = NN - phi_cand + one
      def disc = sum_pq * sum_pq - four * NN
      if disc < zero {
         continue
      }
      if !_wiener_square_residue_filter(disc) {
         continue
      }
      def s = isqrt(disc)
      if (s * s) != disc {
         continue
      }
      def p_cand = (sum_pq + s) / two
      def q_cand = (sum_pq - s) / two
      if (p_cand * q_cand) == NN {
         def p_final = (p_cand > q_cand ? p_cand : q_cand)
         def q_final = (p_cand > q_cand ? q_cand : p_cand)
         return [d_cand, p_final, q_final]
      }
   }
   _wiener_small_d_fallback(NN, ee)
}

fn wiener_attack_entry(any n, any e) any {
   "Classic Wiener attack entrypoint."
   wiener_attack(n, e)
}

fn _wl_abs(any x) any { bigint_lt(x, Z(0)) ? (-x) : x }

fn _wl_try_small_d(any n, any e, any k, any d) any {
   if k == Z(0) || d <= Z(0) { return nil }
   if ((e * d - Z(1)) % k) != Z(0) { return nil }
   if power_mod(power_mod(Z(2), e, n), d, n) != Z(2) { return nil }
   def phi = (e * d - Z(1)) / k
   def facs = factor_from_phi(n, phi)
   if facs == nil { return nil }
   [facs[0], facs[1], d]
}

fn wiener_lattice_attack(any n, any e) any {
   "Nguyen-style lattice Wiener attack. Returns [p, q, d] or nil."
   def s = isqrt(n)
   if s <= Z(0) { return nil }
   def basis = Matrix([[Z(e), Z(s)], [Z(n), Z(0)]])
   def reduced = lll(basis)
   if reduced == nil { return nil }
   mut i = 0
   while i < int(reduced[0]) {
      def row = reduced[2][i]
      def d = _wl_abs(row[1] / s)
      def k = _wl_abs((row[0] - Z(e) * d) / Z(n))
      def hit = _wl_try_small_d(Z(n), Z(e), k, d)
      if hit != nil { return hit }
      i += 1
   }
   nil
}

fn wiener_attack_lattice_entry(any n, any e) any {
   "Lattice Wiener variant entrypoint."
   wiener_lattice_attack(n, e)
}

fn _recover_d_with_wiener_fallback(any n, any e, any p, any q) any {
   def d = compute_d(e, compute_phi(p, q))
   if d != nil && d > 0 { return d }
   if e <= 0 { return nil }
   def w = wiener_attack(n, e)
   if w != nil { return w[0] }
   nil
}

fn wiener_attack_common_prime(any n1, any e1, any c1, any n2, any e2=0, any c2=0) any {
   "Recover from a shared-prime RSA setup, preferring direct shared-prime
   recovery and falling back to Wiener when one side still needs d recovery.
   Returns [m1, m2, p, q1, q2, d1, d2] or nil."
   def direct = common_prime_recover_pair(n1, e1, c1, n2, e2, c2)
   if direct != nil { return direct }
   def fp = common_prime_factor_pair(n1, n2)
   if fp == nil { return nil }
   def p = fp[0]
   def q1 = fp[1]
   def q2 = fp[2]
   def d1 = _recover_d_with_wiener_fallback(n1, e1, p, q1)
   def d2 = e2 > 0 ? _recover_d_with_wiener_fallback(n2, e2, p, q2) : nil
   if d1 == nil || d1 <= 0 { return nil }
   if e2 > 0 && (d2 == nil || d2 <= 0) { return nil }
   def m1, m2 = power_mod(c1, d1, n1), (e2 > 0 && c2 != 0) ? power_mod(c2, d2, n2) : nil
   [m1, m2, p, q1, q2, d1, d2]
}

fn wiener_attack_common_prime_entry(any n1, any e1, any c1, any n2, any e2=0, any c2=0) any {
   "Shared-prime Wiener recovery entrypoint."
   wiener_attack_common_prime(n1, e1, c1, n2, e2, c2)
}
