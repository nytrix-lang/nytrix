;; Keywords: rsa known-d math crypto
;; RSA recovery from known private exponent routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.known_d(known_d_factorize)
use std.math.nt

fn _known_d_factor_pair_if_valid(any n, any e, any d, any p) any {
   def nn = Z(n)
   def pp = Z(p)
   if(pp <= Z(1) || pp >= nn || nn % pp != Z(0)){ return nil }
   def q = nn / pp
   def phi = (pp - Z(1)) * (q - Z(1))
   if(phi <= Z(0)){ return nil }
   if((Z(e) * Z(d) - Z(1)) % phi != Z(0)){ return nil }
   (pp < q) ? [pp, q] : [q, pp]
}

fn known_d_factorize(any n, any e, any d) any {
   "Factor modulus n given public exponent e and private exponent d.
   Uses the probabilistic algorithm based on e*d - 1 = k*phi(n).
   Returns [p, q] or nil."
   def one = Z(1)
   def two = Z(2)
   def k_orig = e * d - one
   mut k, t = k_orig, 0
   while(k % two == Z(0)){
      k = k >> one
      t += 1
   }
   def bases = [Z(2), Z(3), Z(5), Z(7), Z(11), Z(13), Z(17), Z(19), Z(23), Z(29), Z(31), Z(37)]
   mut bi = 0
   while(bi < bases.len){
      def g = bases.get(bi)
      if(g >= n){
         bi += 1
         continue
      }
      def direct = gcd(g, n)
      if(!bigint_eq(direct, Z(0)) && !bigint_eq(direct, one) && !bigint_eq(direct, n)){
         def pair = _known_d_factor_pair_if_valid(n, e, d, direct)
         if(pair != nil){ return pair }
      }
      mut y = power_mod(g, k, n)
      if(y == one || y == n - one){
         bi += 1
         continue
      }
      mut i = 0
      while(i < t){
         def x = power_mod(y, two, n)
         if(x == one){
            def p = gcd(y - one, n)
            if(!bigint_eq(p, Z(0)) && !bigint_eq(p, one) && !bigint_eq(p, n)){
               def pair = _known_d_factor_pair_if_valid(n, e, d, p)
               if(pair != nil){ return pair }
            }
            break
         }
         if(x == n - one){ break }
         y = x
         i += 1
      }
      bi += 1
   }
   nil
}
