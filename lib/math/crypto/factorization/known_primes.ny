;; Keywords: factorization known-primes math crypto number-theory
;; Integer-factorization routines for factorization from known or partially known primes.
;; Includes reusable small-prime, Mersenne-prime, and candidate-GCD scans.
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.known_primes(factor_by_known_primes, factor_moduli_by_known_primes, factor_small_prime_q, factor_mersenne_prime_modulus, factor_by_prime_file, factor_moduli_by_prime_file, factor_smallq, factor_mersenne_primes, factor_system_primes_gcd)
use std.math.nt
use std.os (file_read)
use std.core.str (split, strip)

fn _z(any x) any { is_bigint(x) ? x : Z(x) }

fn _is_nontrivial_factor(any g, any n) bool {
   def gz, nz = _z(g), _z(n)
   gz > 1 && gz < nz && nz % gz == 0
}

fn factor_by_known_primes(any n, any prime_candidates, bool require_semiprime=true) any {
   "Try factoring n by gcd against known prime candidates.
   Returns [p, q] or nil."
   def nz = _z(n)
   if !is_list(prime_candidates) || prime_candidates.len == 0 { return nil }
   mut i = 0
   while i < prime_candidates.len {
      def pc = _z(prime_candidates.get(i, 0))
      if pc > 1 {
         def p = gcd(nz, pc)
         if _is_nontrivial_factor(p, nz) {
            def q = nz / p
            if !require_semiprime || is_prime(q) { return [p, q] }
         }
      }
      i += 1
   }
   nil
}

fn factor_moduli_by_known_primes(any moduli, any prime_candidates, bool require_semiprime=true) list {
   "Batch mode for multiple moduli. Returns [index, p, q] hits."
   if !is_list(moduli) || moduli.len == 0 { return [] }
   mut out = []
   mut i = 0
   while i < moduli.len {
      def n = _z(moduli.get(i, 0))
      def hit = factor_by_known_primes(n, prime_candidates, require_semiprime)
      if hit != nil { out = out.append([i, hit[0], hit[1]]) }
      i += 1
   }
   out
}

fn factor_small_prime_q(any n, any bound=100000, bool require_semiprime=true) any {
   "Try factorization assuming q is a small prime <= bound."
   def nz = _z(n)
   if nz <= 1 { return nil }
   mut p = Z(2)
   def lim = _z(bound)
   while p <= lim {
      if _is_nontrivial_factor(p, nz) {
         def q = nz / p
         if !require_semiprime || is_prime(q) { return [p, q] }
      }
      p = next_prime(p)
   }
   nil
}

fn _default_mersenne_exponents() list {
   [2, 3, 5, 7, 13, 17, 19, 31, 61, 89, 107, 127, 521, 607, 1279, 2203, 2281, 3217,
      4253, 4423, 9689, 9941, 11213, 19937, 21701, 23209, 44497, 86243, 110503,
      132049, 216091, 756839, 859433, 1257787, 1398269, 2976221, 3021377, 6972593,
      13466917, 20336011, 24036583, 25964951, 30402457, 32582657, 37156667, 42643801,
      43112609, 57885161, 74207281, 77232917, 82589933]
}

fn factor_mersenne_prime_modulus(any n, any exponents=nil, bool require_semiprime=true) any {
   "Try factoring n where one factor is a Mersenne prime 2^k - 1.
   Uses known Mersenne-prime exponents list."
   def nz = _z(n)
   if nz <= 1 { return nil }
   def exps = (exponents == nil) ? _default_mersenne_exponents() : exponents
   def max_bits = bit_length(nz)
   mut i = 0
   while i < exps.len {
      def k = int(exps.get(i, 0))
      if k > max_bits { break }
      if k >= 2 {
         def m = bigint_lshift(Z(1), k) - Z(1)
         if _is_nontrivial_factor(m, nz) {
            def q = nz / m
            if !require_semiprime || is_prime(q) { return [m, q] }
         }
      }
      i += 1
   }
   nil
}

fn _parse_prime_lines(any text) list {
   if !is_str(text) || text.len == 0 { return [] }
   mut out = []
   def lines = split(text, "\n")
   mut i = 0
   while i < lines.len {
      def s = strip(to_str(lines.get(i, "")))
      if s.len > 0 {
         def p = _z(s)
         if p > 1 { out = out.append(p) }
      }
      i += 1
   }
   out
}

fn factor_by_prime_file(any n, str path, bool require_semiprime=true) any {
   "Load newline-delimited prime candidates from file and factor n."
   match file_read(path) {
      ok(text) -> {
         def ps = _parse_prime_lines(text)
         if ps.len == 0 { return nil }
         factor_by_known_primes(n, ps, require_semiprime)
      }
      err(ignorederr) -> { ignorederr nil }
   }
}

fn factor_moduli_by_prime_file(any moduli, str path, bool require_semiprime=true) list {
   "Batch mode using newline-delimited prime candidates from file."
   match file_read(path) {
      ok(text) -> {
         def ps = _parse_prime_lines(text)
         if ps.len == 0 { return [] }
         factor_moduli_by_known_primes(moduli, ps, require_semiprime)
      }
      err(ignorederr) -> { ignorederr [] }
   }
}

fn factor_smallq(any n, any bound=100000, bool require_semiprime=true) any {
   "Alias for small-q factorization."
   factor_small_prime_q(n, bound, require_semiprime)
}

fn factor_mersenne_primes(any n, any exponents=nil, bool require_semiprime=true) any {
   "Stable alias for Mersenne-prime factorization."
   factor_mersenne_prime_modulus(n, exponents, require_semiprime)
}

fn factor_system_primes_gcd(any n, any prime_candidates, bool require_semiprime=true) any {
   "Stable alias for scanning a caller-provided prime table with gcd."
   factor_by_known_primes(n, prime_candidates, require_semiprime)
}
