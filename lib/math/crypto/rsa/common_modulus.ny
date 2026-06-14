;; Keywords: rsa common-modulus math crypto
;; RSA common-modulus RSA recovery routines.
;; Recovers m from (N, e1, c1) and (N, e2, c2) via extended GCD.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.common_modulus(common_modulus_attack, common_modulus_attack_root, common_modulus_attack_report, common_modulus_scan_same_n, common_modulus_related_message_attack, common_modulus_related_message_scan, same_n_huge_e_attack, same_n_huge_e_scan)
use std.math.nt

fn common_modulus_attack_root(number N, number e1, number c1, number e2, number c2) any {
   "Recover plaintext m from two ciphertexts under the same modulus N.
   If gcd(e1,e2)=g>1, this requires m^g < N so the final integer g-th root is exact."
   def egcd_result = extended_gcd(e1, e2)
   def g = egcd_result.get(0)
   def a_coeff = egcd_result.get(1)
   def b_coeff = egcd_result.get(2)
   mut c1_mod, c2_mod = c1, c2
   mut a_use, b_use = a_coeff, b_coeff
   if a_coeff < 0 {
      if gcd(c1, N) != 1 { return nil }
      c1_mod = inverse_mod(c1, N)
      a_use = 0 - a_coeff
   }
   if b_coeff < 0 {
      if gcd(c2, N) != 1 { return nil }
      c2_mod = inverse_mod(c2, N)
      b_use = 0 - b_coeff
   }
   def part1, part2 = power_mod(c1_mod, a_use, N), power_mod(c2_mod, b_use, N)
   mut m = (part1 * part2) % N
   if g != 1 {
      def g_int = bigint_to_int(Z(g))
      if g_int <= 0 { return nil }
      def root = nth_root(m, g_int)
      if bigint_pow(root, Z(g_int)) != m { return nil }
      m = root
   }
   m
}

fn common_modulus_attack_report(number N, number e1, number c1, number e2, number c2) dict {
   "Explain a common-modulus recovery attempt.
   Returns a report with gcd(e1,e2), Bezout coefficients, invertibility checks,
   root requirements, recovered plaintext when successful, and a failure reason."
   def eg = extended_gcd(e1, e2)
   def g = eg.get(0)
   def a = eg.get(1)
   def b = eg.get(2)
   def needs_c1_inverse = a < 0
   def needs_c2_inverse = b < 0
   def c1_invertible = gcd(c1, N) == 1
   def c2_invertible = gcd(c2, N) == 1
   mut reason = ""
   mut m = nil
   if needs_c1_inverse && !c1_invertible {
      reason = "c1 is not invertible modulo N"
   } else if needs_c2_inverse && !c2_invertible {
      reason = "c2 is not invertible modulo N"
   } else {
      m = common_modulus_attack_root(N, e1, c1, e2, c2)
      if m == nil {
         reason = g == 1 ? "recovery failed" : "g-th root was not exact"
      }
   }
   {
      "ok": m != nil,
      "plaintext": m,
      "reason": reason,
      "n": N,
      "e1": e1,
      "e2": e2,
      "gcd_exponents": g,
      "bezout": [a, b],
      "needs_c1_inverse": needs_c1_inverse,
      "needs_c2_inverse": needs_c2_inverse,
      "c1_invertible": c1_invertible,
      "c2_invertible": c2_invertible,
      "requires_exact_root": g != 1,
   }
}

fn common_modulus_attack(number N, number e1, number c1, number e2, number c2) any {
   "Recover plaintext m from two ciphertexts encrypted under the same modulus N
   but different public exponents e1, e2."
   common_modulus_attack_root(N, e1, c1, e2, c2)
}

fn _common_modulus_scan_pairs(list entries, bool require_non_coprime=false) list {
   if !is_list(entries) || entries.len < 2 { return [] }
   mut out = []
   mut i = 0
   while i < entries.len {
      def a = entries.get(i, [])
      def n1 = a.get(0, 0)
      def e1 = a.get(1, 0)
      def c1 = a.get(2, 0)
      mut j = i + 1
      while j < entries.len {
         def b = entries.get(j, [])
         def n2 = b.get(0, 0)
         if n1 == n2 {
            def e2 = b.get(1, 0)
            if !require_non_coprime || extended_gcd(e1, e2).get(0, 1) != 1 {
               def c2 = b.get(2, 0)
               mut m = nil
               if require_non_coprime {
                  m = common_modulus_related_message_attack(e1, e2, n1, c1, c2)
               } else {
                  m = common_modulus_attack_root(n1, e1, c1, e2, c2)
               }
               if m != nil { out = out.append([i, j, m]) }
            }
         }
         j += 1
      }
      i += 1
   }
   out
}

fn common_modulus_scan_same_n(list entries) list {
   "Scan list entries of [n, e, c] and recover plaintext candidates for pairs
   sharing the same modulus n.
   Returns list of [i, j, m]."
   _common_modulus_scan_pairs(entries, false)
}

fn same_n_huge_e_attack(number N, number e1, number c1, number e2, number c2) any {
   "Alias for the multi-key common-modulus attack.
   Recovers m when two ciphertexts share N with exponents e1/e2."
   common_modulus_attack_root(N, e1, c1, e2, c2)
}

fn same_n_huge_e_scan(list entries) list {
   "Scanner alias for list entries [n, e, c]."
   common_modulus_scan_same_n(entries)
}

fn common_modulus_related_message_attack(number e1, number e2, number N, number c1, number c2) any {
   "Alias for the related-message common-modulus path.
   This path is meaningful when gcd(e1,e2)=g>1 and m^g < N."
   def eg = extended_gcd(e1, e2)
   def g = eg.get(0, 0)
   if g == 1 { return nil }
   common_modulus_attack_root(N, e1, c1, e2, c2)
}

fn common_modulus_related_message_scan(list entries) list {
   "Related-message scanner for entries [n, e, c].
   Keeps only pairs with gcd(e1,e2)>1."
   _common_modulus_scan_pairs(entries, true)
}
