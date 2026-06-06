;; Keywords: factorization sequence-gcd math crypto number-theory
;; Integer-factorization routines for GCD factorization across generated integer sequences.
;; Reusable scans for sequence-derived non-trivial factors.
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.sequence_gcd(factor_fibonacci_gcd, factor_lucas_gcd, factor_factorial_pm1_gcd, factor_primorial_pm1_gcd, factor_mersenne_pm1_gcd, factor_compositorial_pm1_gcd, factor_fermat_numbers_gcd, factor_multiple_base_inversion_gcd)
use std.math.nt
use std.core.str (str_add, ord_at)

fn _z(any x) any { is_bigint(x) ? x : Z(x) }

fn _pair_sorted(any p, any q) list {
   def pp = _z(p)
   def qq = _z(q)
   (pp <= qq) ? [pp, qq] : [qq, pp]
}

fn _factor_from_candidate(any n, any c) any {
   def nz = _z(n)
   def g = gcd(nz, _z(c))
   if(g > 1 && g < nz && nz % g == 0){ return _pair_sorted(g, nz / g) }
   nil
}

fn _default_bit_cap(any n, int mul=8) int { int(bit_length(_z(n)) * mul + 256) }

fn _digit_value(int ch) int {
   case ch {
      48..57 -> ch - 48
      97..122 -> ch - 87
      65..90 -> ch - 55
      _ -> -1
   }
}

fn _digit_char(int v) str {
   case v {
      0..9 -> chr(48 + v)
      _ -> chr(87 + v)
   }
}

fn _rev_str(str s) str {
   mut out = ""
   mut i = s.len - 1
   while(i >= 0){
      out = str_add(out, chr(ord_at(s, i)))
      i -= 1
   }
   out
}

fn _parse_bigint_base(any s, int base) any {
   if(!is_str(s) || s.len == 0 || base < 2 || base > 36){ return nil }
   mut out = Z(0)
   def bz = _z(base)
   mut i = 0
   while(i < s.len){
      def d = _digit_value(ord_at(s, i))
      if(d < 0 || d >= base){ return nil }
      out = out * bz + _z(d)
      i += 1
   }
   out
}

fn _bigint_to_base_str(any x, int base) str {
   if(base < 2 || base > 36){ return "" }
   mut nz = _z(x)
   if(nz == 0){ return "0" }
   def bz = _z(base)
   mut out = ""
   while(nz > 0){
      def rem = nz % bz
      out = str_add(out, _digit_char(bigint_to_int(rem)))
      nz = nz / bz
   }
   _rev_str(out)
}

fn _reverse_repr_big(any x, int base) any {
   def s = _bigint_to_base_str(_z(x), base)
   def rs = _rev_str(s)
   _parse_bigint_base(rs, base)
}

fn factor_fibonacci_gcd(any n, int limit=10000, any max_bits=nil) any {
   "Try factoring n by gcd(F_k, n) for 1 <= k <= limit.
   Returns [p, q] or nil."
   def nz = _z(n)
   if(nz <= 3){ return nil }
   def cap = (max_bits == nil) ? _default_bit_cap(nz, 10) : int(max_bits)
   mut f0, f1 = Z(0), Z(1)
   mut k = 1
   while(k <= int(limit)){
      def r = _factor_from_candidate(nz, f1)
      if(r != nil){ return r }
      def f2 = f0 + f1
      f0, f1 = f1, f2
      if(bit_length(f1) > cap){ break }
      k += 1
   }
   nil
}

fn factor_lucas_gcd(any n, int limit=10000, any max_bits=nil) any {
   "Try factoring n by gcd(L_k, n) for Lucas numbers L_1=1, L_2=3.
   Returns [p, q] or nil."
   def nz = _z(n)
   if(nz <= 3){ return nil }
   def cap = (max_bits == nil) ? _default_bit_cap(nz, 10) : int(max_bits)
   mut l0, l1 = Z(2), Z(1)
   mut k = 1
   while(k <= int(limit)){
      def r = _factor_from_candidate(nz, l1)
      if(r != nil){ return r }
      def l2 = l0 + l1
      l0, l1 = l1, l2
      if(bit_length(l1) > cap){ break }
      k += 1
   }
   nil
}

fn factor_factorial_pm1_gcd(any n, int limit=30000, any max_bits=nil) any {
   "Try factoring n by gcd(k! - 1, n) and gcd(k! + 1, n).
   Returns [p, q] or nil."
   def nz = _z(n)
   if(nz <= 3){ return nil }
   def cap = (max_bits == nil) ? _default_bit_cap(nz, 8) : int(max_bits)
   mut f, k = Z(1), 2
   while(k <= int(limit)){
      f = f * _z(k)
      mut r = _factor_from_candidate(nz, f - Z(1))
      if(r != nil){ return r }
      r = _factor_from_candidate(nz, f + Z(1))
      if(r != nil){ return r }
      if(bit_length(f) > cap){ break }
      k += 1
   }
   nil
}

fn factor_primorial_pm1_gcd(any n, int limit=10000, any max_bits=nil) any {
   "Try factoring n by gcd(P_k - 1, n) and gcd(P_k + 1, n),
   where P_k is the k-th primorial."
   def nz = _z(n)
   if(nz <= 3){ return nil }
   def cap = (max_bits == nil) ? _default_bit_cap(nz, 8) : int(max_bits)
   mut p = Z(1)
   mut primorial = Z(1)
   mut k = 0
   while(k < int(limit)){
      p = next_prime(p)
      primorial = primorial * p
      mut r = _factor_from_candidate(nz, primorial - Z(1))
      if(r != nil){ return r }
      r = _factor_from_candidate(nz, primorial + Z(1))
      if(r != nil){ return r }
      if(bit_length(primorial) > cap){ break }
      k += 1
   }
   nil
}

fn factor_mersenne_pm1_gcd(any n) any {
   "Try factoring n by gcd(2^k - 1, n) and gcd(2^k + 1, n) for k >= 2."
   def nz = _z(n)
   if(nz <= 3){ return nil }
   mut k = 2
   def lim = int(bit_length(nz)) + 2
   while(k <= lim){
      def p2 = bigint_lshift(Z(1), k)
      mut r = _factor_from_candidate(nz, p2 - Z(1))
      if(r != nil){ return r }
      r = _factor_from_candidate(nz, p2 + Z(1))
      if(r != nil){ return r }
      k += 1
   }
   nil
}

fn factor_compositorial_pm1_gcd(any n, int limit=10000) any {
   "Try factoring n with compositorial-style gcd(F ± 1, n),
   where F incrementally removes prime powers from factorial growth."
   def nz = _z(n)
   if(nz <= 3){ return nil }
   mut F, p = Z(1), Z(2)
   mut x = 2
   while(x < int(limit)){
      F = F * _z(x)
      while(F % p == 0){ F, p = F / p, next_prime(p) }
      mut r = _factor_from_candidate(nz, F - Z(1))
      if(r != nil){ return r }
      r = _factor_from_candidate(nz, F + Z(1))
      if(r != nil){ return r }
      x += 1
   }
   nil
}

fn factor_fermat_numbers_gcd(any n, int limit=30) any {
   "Try factoring n by gcd(F_k, n), where F_k = 2^(2^k) + 1."
   def nz = _z(n)
   if(nz <= 3){ return nil }
   mut k = 2
   while(k < int(limit)){
      def f, r = bigint_lshift(Z(1), (1 << k)) + Z(1), _factor_from_candidate(nz, f)
      if(r != nil){ return r }
      k += 1
   }
   nil
}

fn factor_multiple_base_inversion_gcd(any n, int max_pow=5) any {
   "Try factoring n using reversed representations of n^p across bases 10/2/8/16,
   plus gcd with n xor reversed-value."
   def nz = _z(n)
   if(nz <= 3){ return nil }
   mut p = 1
   while(p <= int(max_pow)){
      def np = bigint_pow(nz, _z(p))
      def cands = [
         _reverse_repr_big(np, 10),
         _reverse_repr_big(np, 2),
         _reverse_repr_big(np, 8),
         _reverse_repr_big(np, 16)
      ]
      mut i = 0
      while(i < cands.len){
         def cand = cands[i]
         if(cand != nil){
            mut r = _factor_from_candidate(nz, cand)
            if(r != nil){ return r }
            def mix = bigint_xor(nz, cand)
            r = _factor_from_candidate(nz, mix)
            if(r != nil){ return r }
         }
         i += 1
      }
      p += 1
   }
   nil
}
