;; Keywords: rsa extended-wiener
;; RSA extended Wiener RSA attack routines.
;; References:
;; - Dujella A., "Continued fractions and RSA with small secret exponent"
;; - Nguyen P. Q., "Public-Key Cryptanalysis"
module std.math.crypto.rsa.extended_wiener(extended_wiener_attack, lattice_wiener_attack, extended_wiener_attack_entry, wiener_attack_lattice_entry)
use std.math.nt
use std.math.crypto.factorization.known_phi
use std.math.crypto.lattice.lll
use std.math.matrix

fn _babs(any: x): any { bigint_lt(x, Z(0)) ? (-x) : x }

fn _rsa_try_small_d(any: n, any: e, any: k, any: d): any {
   if(bigint_eq(k, Z(0)) || bigint_le(d, Z(0))){ return nil }
   if(bigint_eq((e * d - Z(1)) % k, Z(0)) == false){ return nil }
   if(power_mod(power_mod(Z(2), e, n), d, n) != Z(2)){ return nil }
   def phi = (e * d - Z(1)) / k
   def factors = factor_from_phi(n, phi)
   if(factors == nil){ return nil }
   def p, q = factors.get(0), factors.get(1)
   [p, q, d]
}

fn extended_wiener_attack(any: n, any: e, int: max_s=20000, int: max_r=100, int: max_t=100): any {
   "Dujella's extension of Wiener's attack.
   Returns [p, q, d] or nil."
   def convs = cf_convergents(continued_fraction(e, n))
   if(convs.len < 3){ return nil }
   mut m = 1
   while(m + 2 < convs.len){
      def c0, c1 = convs.get(m), convs.get(m + 1)
      def c2 = convs.get(m + 2)
      def k0 = Z(c0.get(0))
      def d0 = Z(c0.get(1))
      def k1 = Z(c1.get(0))
      def d1 = Z(c1.get(1))
      def k2 = Z(c2.get(0))
      def d2 = Z(c2.get(1))
      mut s = 0
      while(s < max_s){
         mut r = 0
         while(r < max_r){
            def rk, rd = Z(r) * k0 + Z(s) * k1, Z(r) * d0 + Z(s) * d1
            def hit = _rsa_try_small_d(n, e, rk, rd)
            if(hit != nil){ return hit }
            r += 1
         }
         mut t = 0
         while(t < max_t){
            def tk, td = Z(s) * k2 - Z(t) * k1, Z(s) * d2 - Z(t) * d1
            def hit = _rsa_try_small_d(n, e, tk, td)
            if(hit != nil){ return hit }
            t += 1
         }
         s += 1
      }
      m += 2
   }
   nil
}

fn lattice_wiener_attack(any: n, any: e): any {
   "Nguyen's lattice variant for small-d RSA.
   Returns [p, q, d] or nil."
   def s = isqrt(n)
   if(bigint_le(s, Z(0))){ return nil }
   def basis = Matrix([[e, s], [n, Z(0)]])
   def reduced = lll(basis)
   def rows = _matrix_rows(reduced)
   mut i = 0
   while(i < rows){
      def row = _matrix_data(reduced).get(i)
      def d = _babs(row.get(1) / s)
      def k = _babs((row.get(0) - e * d) / n)
      def hit = _rsa_try_small_d(n, e, k, d)
      if(hit != nil){ return hit }
      i += 1
   }
   nil
}

fn extended_wiener_attack_entry(any: n, any: e, int: max_s=20000, int: max_r=100, int: max_t=100): any {
   "Extended Wiener attack entrypoint."
   extended_wiener_attack(n, e, max_s, max_r, max_t)
}

fn wiener_attack_lattice_entry(any: n, any: e): any {
   "Nguyen lattice Wiener variant entrypoint."
   lattice_wiener_attack(n, e)
}
