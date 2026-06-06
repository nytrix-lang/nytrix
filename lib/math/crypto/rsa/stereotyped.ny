;; Keywords: rsa stereotyped math crypto
;; RSA stereotyped-message attacks routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.stereotyped(stereotyped_solve, stereotyped_message_attack)
use std.math.nt
use std.math.crypto.poly (poly_set_at)
use std.math.crypto.number.partial
use std.math.crypto.lattice.howgrave_graham
use std.math.crypto.lattice.coppersmith

def _STEREOTYPED_DIRECT_BITS = 20

fn _sm_build_linear(any pi) any {
   "Build known + x*2^off for a PartialInteger with exactly one unknown chunk.
   Returns [poly_coeffs, bound] or nil."
   def bounds = partial_integer_unknown_bounds(pi)
   if(bounds.len != 1){ return nil }
   def ku = partial_integer_known_and_unknowns(pi)
   def known = ku.get(0)
   def offs = ku.get(1)
   def lens = ku.get(2)
   if(offs.len != 1 || lens.len != 1){ return nil }
   def off = offs.get(0)
   def bits = lens.get(0)
   [[known, bigint_lshift(Z(1), off)], bigint_lshift(Z(1), bits)]
}

fn _stereotyped_direct_scan(any n, int e, any c, any known_msg, int unknown_bits) any {
   if(unknown_bits < 0 || unknown_bits > _STEREOTYPED_DIRECT_BITS){ return nil }
   def nn = Z(n)
   def cc = mod(Z(c), nn)
   def kk = Z(known_msg)
   def ez = Z(e)
   if(unknown_bits <= 30){
      def limit_i = 1 << unknown_bits
      mut x_i = 0
      while(x_i < limit_i){
         def msg = kk + Z(x_i)
         if(power_mod(msg, ez, nn) == cc){ return msg }
         x_i += 1
      }
      return nil
   }
   def limit = bigint_lshift(Z(1), unknown_bits)
   mut x = Z(0)
   while(x < limit){
      def msg = kk + x
      if(power_mod(msg, ez, nn) == cc){ return msg }
      x += Z(1)
   }
   nil
}

fn stereotyped_solve(number n, int e, number c, number known_msg, int unknown_bits) any {
   "Coppersmith's stereotyped message attack: recover m = known_msg + x
   where x < 2^unknown_bits and(known_msg + x)^e = c(mod n).
   Uses polynomial root-finding via LLL.  Returns m or nil."
   def direct = _stereotyped_direct_scan(n, e, c, known_msg, unknown_bits)
   if(direct != nil){ return direct }
   def p_init = [known_msg, 1]
   mut list: f = poly_pow(p_init, e)
   def c_val = f.get(0)
   poly_set_at(f, 0, (c_val - c) % n)
   def X = bigint_lshift(Z(1), unknown_bits)
   def roots = coppersmith_univariate(f, n, X)
   if(roots.len > 0){
      def x0 = roots.get(0)
      return(known_msg + x0)
   }
   nil
}

fn stereotyped_message_attack(number n, int e, number c, any partial_m, int m=1, int t=0) any {
   "Recover plaintext from c when the plaintext has one bounded unknown chunk.
   partial_m must be a PartialInteger with exactly one unknown segment."
   def lin = _sm_build_linear(partial_m)
   if(lin == nil){ return nil }
   def base_poly = lin.get(0)
   def X = lin.get(1)
   mut list: f = poly_pow(base_poly, e)
   poly_set_at(f, 0, f.get(0) - Z(c))
   def roots = hg_modular_univariate(f, Z(n), m, t, X)
   mut i = 0
   while(i < roots.len){
      def x0 = roots.get(i)
      if(x0 != Z(0)){
         def msg = partial_integer_sub(partial_m, [x0])
         if(msg != nil && power_mod(msg, Z(e), Z(n)) == Z(c)){ return msg }
      }
      i += 1
   }
   nil
}

#main {
   def p = Z(1000003)
   def q = Z(1000033)
   def n = p * q
   def known = Z(12345000)
   def msg = known + Z(7)
   def c = power_mod(msg, Z(3), n)
   assert(stereotyped_solve(n, 3, c, known, 4) == msg, "stereotyped small window direct recovery")
   print("✓ std.math.crypto.rsa.stereotyped self-test passed")
}
