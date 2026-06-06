;; Keywords: rsa recover-modulus math crypto
;; RSA modulus recovery from oracle samples routines.
;; References:
;; - Oracle technique based on gcd(m^e - c) and gcd(enc(m)^2 - enc(m^2)).
module std.math.crypto.rsa.recover_modulus(recover_modulus_known_e, recover_modulus_unknown_e)
use std.math.nt

fn recover_modulus_known_e(fnptr encrypt_fn, any e, list samples=[2, 3, 5, 7]) any {
   "Recover modulus n from oracle encrypt_fn(m)=m^e mod n when e is known."
   if(samples.len == 0){ return nil }
   mut g, i = Z(0), 0
   while(i < samples.len){
      def m, c = Z(samples.get(i)), Z(encrypt_fn(m))
      mut delta = bigint_pow(m, Z(e)) - c
      if(delta < 0){ delta = -delta }
      g = (i == 0) ? delta : gcd(g, delta)
      i += 1
   }
   if(g <= Z(1)){ return nil }
   g
}

fn recover_modulus_unknown_e(fnptr encrypt_fn, list samples=[2, 3, 5, 7]) any {
   "Recover modulus n from textbook RSA encryption oracle without knowing e.
   Uses gcd(enc(m)^2 - enc(m^2))."
   if(samples.len == 0){ return nil }
   mut g, i = Z(0), 0
   while(i < samples.len){
      def m = Z(samples.get(i))
      def c1 = Z(encrypt_fn(m))
      def c2 = Z(encrypt_fn(m * m))
      mut delta = c1 * c1 - c2
      if(delta < 0){ delta = -delta }
      g = (i == 0) ? delta : gcd(g, delta)
      i += 1
   }
   if(g <= Z(1)){ return nil }
   g
}
