;; Keywords: factorization chunked-primes math crypto number-theory
;; Integer-factorization routines for structured-prime factorization for chunked or limb-shaped primes.
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.chunked_primes(chunked_prime_factor, chunked_prime_factor_params, chunked_prime_factor_with_limb_factors, chunked_low_limb_factors, factor_chunked_primes)
use std.core
use std.math.nt (Z, bit_length, gcd, inverse_mod, factor, factordb_factor, is_prime, next_prime)
use std.math.crypto.factorization.ecm as ecm
use std.math.crypto.factorization.pollard as pollard

fn _chunked_product(list facs) any {
   mut prod = Z(1)
   mut i = 0
   while(i < facs.len){
      def ent = facs.get(i)
      mut e = int(ent.get(1))
      while(e > 0){
         prod = prod * Z(ent.get(0))
         e -= 1
      }
      i += 1
   }
   prod
}

fn _chunked_factor_low_limb(any n) any {
   def nn = Z(n)
   def fd = factordb_factor(nn, false)
   if(is_list(fd)){
      if(_chunked_product(fd) == nn){ return fd }
   }
   _chunked_factor_local(nn)
}

fn _chunked_pack_flat(list flat) list {
   mut out = []
   mut i = 0
   while(i < flat.len){
      def p = Z(flat[i])
      mut j, e = 0, 0
      while(j < flat.len){
         if(Z(flat[j]) == p){ e += 1 }
         j += 1
      }
      mut seen, k = false, 0
      while(k < out.len){
         if(Z(out[k][0]) == p){ seen = true }
         k += 1
      }
      if(!seen){ out = out.append([p, e]) }
      i += 1
   }
   out
}

fn _chunked_trial_factor(any n, int bound) any {
   def nn = Z(n)
   if(nn % Z(2) == Z(0)){ return Z(2) }
   mut p = Z(3)
   def lim = Z(bound)
   while(p <= lim && p * p <= nn){
      if(nn % p == Z(0)){ return p }
      p = next_prime(p)
   }
   nil
}

fn _chunked_split_once(any n) any {
   def nn = Z(n)
   def small = _chunked_trial_factor(nn, 200000)
   if(small != nil){ return small }
   def pm1 = pollard.pollard_pm1_stage2(nn, 2000, 50000)
   if(pm1 != nil && pm1 > Z(1) && pm1 < nn){ return pm1 }
   def pp1 = pollard.williams_pp1(nn, 20000)
   if(pp1 != nil && pp1 > Z(1) && pp1 < nn){ return pp1 }
   def ec = ecm.montgomery_ecm_factor(nn, 1000, 32, 5000)
   if(ec != nil && ec > Z(1) && ec < nn){ return ec }
   pollard.pollard_brent(nn, Z(2), Z(1), 128, 50000)
}

fn _chunked_factor_flat_rec(any n, list out) list {
   def nn = Z(n)
   if(nn == Z(1)){ return out }
   if(is_prime(nn)){ return out.append(nn) }
   if(bit_length(nn) <= 192){ return _chunked_factor_flat_from_factor(factor(nn, false, false), out) }
   def f = _chunked_split_once(nn)
   if(f == nil || f <= Z(1) || f >= nn){ return out.append(nn) }
   def a = _chunked_factor_flat_rec(f, out)
   _chunked_factor_flat_rec(nn / f, a)
}

fn _chunked_factor_flat_from_factor(list facs, list out) list {
   mut acc = out
   mut i = 0
   while(i < facs.len){
      def ent = facs[i]
      mut e = int(ent[1])
      while(e > 0){
         acc = acc.append(Z(ent[0]))
         e -= 1
      }
      i += 1
   }
   acc
}

fn _chunked_factor_local(any n) any {
   def flat = _chunked_factor_flat_rec(n, [])
   def packed = _chunked_pack_flat(flat)
   _chunked_product(packed) == Z(n) ? packed : nil
}

fn chunked_low_limb_factors(any n, int limb_bits=512) any {
   "Return the factorization used for the public low limb `n mod 2^limb_bits`."
   _chunked_factor_low_limb(Z(n) % (Z(1) << Z(limb_bits)))
}

fn _chunked_append_divisors(list facs, int i, any a, any bound, int lo, int hi, list out) list {
   if(a > bound){ return out }
   if(i == facs.len){
      def bits = bit_length(a)
      return(lo <= bits && bits <= hi) ? out.append(a) : out
   }
   def f = facs.get(i)
   def p, k = f.get(0), int(f.get(1))
   mut acc, x, j = out, Z(1), 0
   while(j <= k){
      if(a * x > bound){ break }
      acc = _chunked_append_divisors(facs, i + 1, a * x, bound, lo, hi, acc)
      x = x * p
      j += 1
   }
   acc
}

fn _chunked_try_low(any n, any R, any B, any t, any n1, any n2, any cp, int min_factor_bits) any {
   mut cpl, cq = cp, t / cp
   if(cpl > cq){
      cpl = t / cp
      cq = cp
   }
   def g = gcd(cpl, cq)
   def a = cpl / g
   def b = cq / g
   if(a == Z(1)){ return nil }
   def inv = inverse_mod(b, a)
   if(inv == Z(0)){ return nil }
   mut C1 = 0
   while(C1 <= 1){
      def rhs = n1 + Z(C1) * R
      if(rhs % g == Z(0)){
         mut bp = ((rhs / g) * inv) % a
         while(bp < B){
            def rhsb = rhs - cq * bp
            if(rhsb % cpl == Z(0)){
               def bq = rhsb / cpl
               if(Z(0) <= bq && bq < B){
                  mut C2 = 0
                  while(C2 <= 2){
                     def rhs2 = n2 - (bp * bq + Z(C1)) + Z(C2) * R
                     if(rhs2 % g == Z(0)){
                        mut ap = ((rhs2 / g) * inv) % a
                        while(ap < B){
                           def rhsa = rhs2 - ap * cq
                           if(rhsa % cpl == Z(0)){
                              def aq = rhsa / cpl
                              if(Z(0) <= aq && aq < B){
                                 def p = ap * R * R + bp * R + cpl
                                 if(bit_length(p) >= min_factor_bits && n % p == Z(0)){ return [p, n / p] }
                              }
                           }
                           ap = ap + a
                        }
                     }
                     C2 += 1
                  }
               }
            }
            bp = bp + a
         }
      }
      C1 += 1
   }
   nil
}

fn chunked_prime_factor_params(any n, int limb_bits, int small_limb_bits, any windows, int min_factor_bits) any {
   "Recover RSA factors when p and q are built from R=2^limb_bits chunks and both low chunks multiply without carry.
   FactorDB is tried for the public low limb first ; local fallback stays bounded for small limbs."
   def nn = Z(n)
   def R = Z(1) << Z(limb_bits)
   def B = Z(1) << Z(small_limb_bits)
   def t = nn % R
   def facs = _chunked_factor_low_limb(t)
   if(facs == nil || facs.len == 0){ return nil }
   chunked_prime_factor_with_limb_factors(nn, facs, limb_bits, small_limb_bits, windows, min_factor_bits)
}

fn chunked_prime_factor_with_limb_factors(
   any n, list facs, int limb_bits=512, int small_limb_bits=256, any windows=nil, int min_factor_bits=1000
) any {
   "Recover chunked-prime RSA factors using a supplied factorization of `n mod 2^limb_bits`."
   def nn = Z(n)
   def R = Z(1) << Z(limb_bits)
   def B = Z(1) << Z(small_limb_bits)
   def t = nn % R
   if(_chunked_product(facs) != t){ return nil }
   def n1 = (nn / R) % R
   def n2 = (nn / (R * R)) % R
   def ranges = windows == nil ? [[248, 264], [240, 272], [232, 280], [224, 288], [1, small_limb_bits]] : windows
   mut wi = 0
   while(wi < ranges.len){
      def w = ranges.get(wi)
      def candidates = _chunked_append_divisors(facs, 0, Z(1), B, int(w.get(0)), int(w.get(1)), [])
      mut i = 0
      while(i < candidates.len){
         def ans = _chunked_try_low(nn, R, B, t, n1, n2, candidates.get(i), min_factor_bits)
         if(ans != nil){ return ans }
         i += 1
      }
      wi += 1
   }
   nil
}

fn chunked_prime_factor(any n) any {
   "Recover RSA factors for the common 512-bit limb / 256-bit low-chunk construction."
   chunked_prime_factor_params(n, 512, 256, [[248, 264], [240, 272], [232, 280], [224, 288], [1, 256]], 1000)
}

fn factor_chunked_primes(any n) any {
   "Alias for chunked_prime_factor."
   chunked_prime_factor(n)
}
