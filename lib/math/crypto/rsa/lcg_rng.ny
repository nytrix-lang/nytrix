;; Keywords: rsa lcg-rng
;; RSA prime recovery from LCG-generated randomness routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
module std.math.crypto.rsa.lcg_rng(rsa_lcg_factor_power2)
use std.core
use std.math.nt
use std.math.crypto.number.arith (two_adic_valuation, mod_sqrt_power2)

fn rsa_lcg_factor_power2(any: n, any: mult, any: inc, int: modulus_bits=512, int: max_gap=4096): list {
   "Recover [p, q, gap] when p*q == n and q follows p after gap LCG steps modulo 2^modulus_bits.
   The LCG is x -> mult*x + inc mod 2^modulus_bits. Returns [] if not found."
   def nn = Z(n)
   def M = Z(1) << modulus_bits
   def A = mod(Z(mult), M)
   def B = mod(Z(inc), M)
   mut a_pow = A
   mut geom = Z(1)
   mut gap = 1
   while(gap <= max_gap){
      def b = mod(B * geom, M)
      if(two_adic_valuation(b, modulus_bits) > 0){
         def half_b = b / Z(2)
         def inv_a = inverse_mod(a_pow, M)
         if(inv_a != 0 && inv_a != nil){
            def s = mod(inv_a * inv_a * half_b * half_b + inv_a * mod(nn, M), M)
            def roots = mod_sqrt_power2(s, modulus_bits, 8192)
            def center = mod(inv_a * half_b, M)
            mut i = 0
            while(i < roots.len){
               def p = mod(roots.get(i) - center, M)
               if(p > Z(1) && nn % p == Z(0)){
                  def q = nn / p
                  if(is_prime(p) && is_prime(q)){ return [p, q, gap] }
               }
               i += 1
            }
         }
      }
      geom = mod(geom + a_pow, M)
      a_pow = mod(a_pow * A, M)
      gap += 1
   }
   []
}
