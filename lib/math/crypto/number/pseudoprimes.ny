;; Keywords: number-theory pseudoprimes math crypto
;; Crypto number-theory routines for pseudoprime generation and checks.
;; Reference:
;; - "Prime and Prejudice: Primality Testing Under Adversarial Conditions"
;; References:
;; - std.math.crypto.number
;; - std.math.crypto
module std.math.crypto.number.pseudoprimes(generate_pseudoprime)
use std.math.nt
use std.math.crypto.number.crt

fn _pp_index(list xs, any v) int {
   mut i = 0
   while i < xs.len {
      if xs.get(i) == v { return i }
      i += 1
   }
   -1
}

fn _pp_set_add(list xs, any v) list {
   if _pp_index(xs, v) >= 0 { return xs }
   xs.append(v)
}

fn _pp_set_intersect(list a, list b) list {
   mut out = []
   mut i = 0
   while i < a.len {
      def v = a.get(i)
      if _pp_index(b, v) >= 0 { out = out.append(v) }
      i += 1
   }
   out
}

fn _kronecker_minus_one_residues(any a) list {
   mut out = []
   mut p = Z(1)
   def lim = Z(4) * Z(a)
   while p < lim {
      if bigint_mod(p, Z(2)) == Z(1) && kronecker(Z(a), p) == -1 { out = out.append(p) }
      p += Z(1)
   }
   out
}

fn _generate_s(list bases, list ks) list {
   mut all_s = []
   mut i = 0
   while i < bases.len {
      def a = Z(bases.get(i))
      def sa = _kronecker_minus_one_residues(a)
      mut inter = sa
      mut j = 0
      while j < ks.len {
         def ki = Z(ks.get(j))
         assert(gcd(ki, Z(4) * a) == Z(1), "gcd(ki,4a)=1")
         mut shifted = []
         mut t = 0
         while t < sa.len {
            def s = sa.get(t)
            shifted = _pp_set_add(shifted, mod(inverse_mod(ki, Z(4) * a) * (s + ki - Z(1)), Z(4) * a))
            t += 1
         }
         inter = _pp_set_intersect(inter, shifted)
         j += 1
      }
      all_s = all_s.append(inter)
      i += 1
   }
   all_s
}

fn _backtrack_crt(list S, list A, list X, list M, int i) any {
   if i >= S.len { return fast_crt(X, M) }
   def mods = clone(M).append(Z(4) * Z(A.get(i)))
   def opts = S.get(i)
   mut j = 0
   while j < opts.len {
      def xs = clone(X).append(opts.get(j))
      def probe = fast_crt(xs, mods)
      if probe != nil {
         def r = _backtrack_crt(S, A, xs, mods, i + 1)
         if r != nil { return r }
      }
      j += 1
   }
   nil
}

fn generate_pseudoprime(list bases, any k2=nil, any k3=nil, int min_bit_length=0) any {
   "Generate n = p1*p2*p3 passing Miller-Rabin for the provided bases.
   Returns [n,p1,p2,p3] or nil."
   if bases.len <= 0 { return nil }
   def sorted = sort(clone(bases))
   mut kk2, kk3 = (k2 == nil) ? next_prime(Z(sorted.get(sorted.len - 1))) : Z(k2), (k3 == nil) ? next_prime(kk2) : Z(k3)
   mut tries = 0
   while tries < 128 {
      def X, M = [inverse_mod(-kk3, kk2), inverse_mod(-kk2, kk3)], [kk2, kk3]
      def S = _generate_s(sorted, M)
      def zm = _backtrack_crt(S, sorted, X, M, 0)
      if zm != nil {
         def z, m = zm.get(0), zm.get(1)
         mut i = (Z(1) << Z(int(min_bit_length) / 3)) / m
         while i < Z(1) + Z(4096) {
            def p1, p2 = z + i * m, kk2 * (p1 - Z(1)) + Z(1)
            def p3 = kk3 * (p1 - Z(1)) + Z(1)
            if is_prime(p1) && is_prime(p2) && is_prime(p3) { return [p1 * p2 * p3, p1, p2, p3] }
            i += Z(1)
         }
      }
      kk3 = next_prime(kk3)
      tries += 1
   }
   nil
}

#main {
   def r = generate_pseudoprime([Z(2)], Z(3), Z(5), 0)
   if r != nil {
      assert(r.len == 4, "generate_pseudoprime tuple size")
      assert(r.get(0) == r.get(1) * r.get(2) * r.get(3), "generate_pseudoprime product")
   }
   print("✓ std.math.crypto.number.pseudoprimes self-test passed")
}
