;; Keywords: rsa known-crt-exponents math crypto
;; RSA recovery from known CRT exponents routines.
;; References:
;; - Campagna, Sethi, "Key Recovery Method for CRT Implementation of RSA"
module std.math.crypto.rsa.known_crt_exponents(possible_prime_factors_from_crt_exponents, factor_from_known_crt_exponent, factor_from_small_crt_exponent_gcd)
use std.math.nt
use std.math.crypto.rsa.op (compute_phi, compute_d)

fn _get_possible_primes(any e, any d_crt) list {
   mut out = list(0)
   def mul = Z(e) * Z(d_crt) - Z(1)
   mut k = Z(3)
   while(k < Z(e)){
      if(mul % k == Z(0)){
         def p = mul / k + Z(1)
         if(is_prime(p)){ out = out.append(p) }
      }
      k += Z(1)
   }
   out
}

fn possible_prime_factors_from_crt_exponents(any e_start, any e_end, any n=nil, any dp=nil, any dq=nil, any p_bits=nil, any q_bits=nil) list {
   "Return candidate factors implied by known dp and/or dq over a public exponent range.
   Each result is either [p], [q], or [p, q]."
   assert(!(dp == nil && dq == nil), "dp or dq required")
   mut out = list(0)
   mut e = Z(e_start)
   while(e < Z(e_end)){
      if(bigint_mod(e, Z(2)) == Z(0)){
         e += Z(1)
         continue
      }
      def p_candidates = (dp == nil) ? nil : _get_possible_primes(e, dp)
      def q_candidates = (dq == nil) ? nil : _get_possible_primes(e, dq)
      if(p_candidates != nil && q_candidates != nil){
         mut i = 0
         while(i < p_candidates.len){
            def p = p_candidates.get(i)
            mut j = 0
            while(j < q_candidates.len){
               def q = q_candidates.get(j)
               if((n == nil || p * q == Z(n)) &&
                  (p_bits == nil || bit_length(p) == p_bits) &&
                  (q_bits == nil || bit_length(q) == q_bits)){
                  out = out.append([p, q])
               }
               j += 1
            }
            i += 1
         }
      } elif(p_candidates != nil){
         mut i = 0
         while(i < p_candidates.len){
            def p = p_candidates.get(i)
            if(p_bits == nil || bit_length(p) == p_bits){
               if(n == nil){ out = out.append([p]) } elif(Z(n) % p == Z(0)){ out = out.append([p, Z(n) / p]) }
            }
            i += 1
         }
      } elif(q_candidates != nil){
         mut i = 0
         while(i < q_candidates.len){
            def q = q_candidates.get(i)
            if(q_bits == nil || bit_length(q) == q_bits){
               if(n == nil){ out = out.append([q]) } elif(Z(n) % q == Z(0)){ out = out.append([q, Z(n) / q]) }
            }
            i += 1
         }
      }
      e += Z(2)
   }
   out
}

fn factor_from_known_crt_exponent(any e, any d_crt, any n) any {
   "Factor RSA modulus n from a known CRT exponent dp=d mod(p-1) or dq=d mod(q-1).
   Since e*d_crt - 1 is a small public-exponent multiple of p-1 or q-1,
   scan k < e and test the implied factor against n. Returns [p, q] or nil."
   def nn = Z(n)
   def mul = Z(e) * Z(d_crt) - Z(1)
   mut k = Z(1)
   while(k < Z(e)){
      if(mul % k == Z(0)){
         def p = mul / k + Z(1)
         if(p > Z(1) && p < nn && nn % p == Z(0)){ return [p, nn / p] }
      }
      k += Z(1)
   }
   nil
}

fn factor_from_small_crt_exponent_gcd(any n, any e, any max_dcrt=(1 << 16), any probe=2) any {
   "Factor RSA modulus n when dp or dq is small but not known.
   For the correct CRT exponent dcrt, (probe^e)^dcrt == probe mod p or q, so
   gcd((probe^e)^dcrt - probe, n) reveals a factor. Returns [p, q, dcrt] or nil."
   def nn, ee, m = Z(n), Z(e), Z(probe)
   def a = power_mod(m, ee, nn)
   mut dcrt = 2
   mut cur = mod_mul(a, a, nn)
   while(dcrt < max_dcrt){
      def g = gcd(bigint_sub(cur, m), nn)
      if(g != Z(1) && g != nn){ return [g, nn / g, dcrt] }
      cur = mod_mul(cur, a, nn)
      dcrt += 1
   }
   nil
}
