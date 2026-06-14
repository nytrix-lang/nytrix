;; Keywords: rsa repeated-roots math crypto
;; RSA repeated-root attacks routines.
;; mod_sqrt_all: Tonelli-Shanks to find all sqrt(a) mod p.
;; mod_2kth_roots: find all 2^k-th roots mod n = pq using CRT.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.repeated_roots(mod_sqrt_all, mod_2kth_roots)
use std.core
use std.math.nt

fn mod_sqrt_all(number a, number p) list {
   "Find all square roots of a mod p using Tonelli-Shanks.
   Returns [r, p-r] if a is a quadratic residue mod p, empty list otherwise."
   def r = tonelli_shanks(a, p)
   if r == Z(-1) { return list(0) }
   def r2 = mod(p - r, p)
   if r == r2 { return [r] }
   [r, r2]
}

fn _list_has(list xs, any x) bool {
   mut i = 0
   while i < xs.len {
      if xs.get(i) == x { return true }
      i += 1
   }
   false
}

fn _find_2kth_roots_prime(number c, int k, number p) list {
   if k <= 0 { return [mod(c, p)] }
   mut roots = [mod(c, p)]
   mut depth = 0
   while depth < k && roots.len > 0 {
      mut next = []
      mut i = 0
      while i < roots.len {
         def srs = mod_sqrt_all(roots.get(i), p)
         mut j = 0
         while j < srs.len {
            def v = srs.get(j)
            if !_list_has(next, v) { next = next.append(v) }
            j += 1
         }
         i += 1
      }
      roots = next
      depth += 1
   }
   roots
}

fn mod_2kth_roots(number c, int k, number p, number q) list {
   "Find all 2^k-th roots of c mod n = p*q using CRT.
   Finds all x such that x^(2^k) = c mod n.
   c: target value, k: exponent(find 2^k-th roots),
   p: first prime, q: second prime.
   Returns list of all 2^k-th roots mod n."
   def roots_p, roots_q = _find_2kth_roots_prime(c, k, p), _find_2kth_roots_prime(c, k, q)
   def n_val = p * q
   def np = roots_p.len
   def nq = roots_q.len
   mut result = []
   def p_inv, q_inv = inverse_mod(p, q), inverse_mod(q, p)
   mut i = 0
   while i < np {
      def a_p = roots_p.get(i)
      mut j = 0
      while j < nq {
         def a_q = roots_q.get(j)
         def root = mod(a_p * q * q_inv + a_q * p * p_inv, n_val)
         if !_list_has(result, root) { result = result.append(root) }
         j += 1
      }
      i += 1
   }
   result
}
