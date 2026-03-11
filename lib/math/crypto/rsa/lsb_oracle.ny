;; Keywords: rsa lsb-oracle
;; RSA LSB-oracle attack routines.
;; Recovers plaintext by querying the least-significant bit of successive
;; doubled plaintexts.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
module std.math.crypto.rsa.lsb_oracle(lsb_oracle_attack, lsb_oracle_recover_from_bits, lsb_oracle_variant_attack)
use std.math.nt
use std.math.crypto.rsa.op (compute_phi, compute_d)

fn lsb_oracle_recover_from_bits(any: n, list: bits): any {
   "Recover plaintext from a recorded RSA parity-oracle bit transcript.
   `bits` must be the oracle answers for c*(2^e)^i mod n, starting at i=1."
   mut low = Z(0)
   mut high = Z(1)
   mut denom = Z(1)
   mut i = 0
   while(i < bits.len){
      def mid = low + high
      denom = denom * Z(2)
      if(bits[i] == 0){
         low = low * Z(2)
         high = mid
      } else {
         low = mid
         high = high * Z(2)
      }
      i += 1
   }
   (low * Z(n) + denom - Z(1)) / denom
}

fn lsb_oracle_attack(any: n, any: e, any: c, fnptr: oracle_fn): any {
   "Recover plaintext from ciphertext c using a parity oracle.
   oracle_fn(ciphertext) must return the last bit of the decrypted plaintext.
   Returns the recovered plaintext integer."
   mut low = Z(0)
   mut high = Z(1)
   mut denom = Z(1)
   mut cc = Z(c)
   def twoe = power_mod(Z(2), Z(e), Z(n))
   while(denom < Z(n) * Z(2)){
      cc = mod(cc * twoe, Z(n))
      def mid = low + high
      denom = denom * Z(2)
      if(oracle_fn(cc) == 0){
         low = low * Z(2)
         high = mid
      } else {
         low = mid
         high = high * Z(2)
      }
   }
   (low * Z(n) + denom - Z(1)) / denom
}

fn lsb_oracle_variant_attack(any: n, any: e, any: c, int: bit_len, fnptr: oracle_fn): any {
   "Recover plaintext from a parity oracle using inverse powers of two.
   oracle_fn(ciphertext) must return the least-significant bit of the decrypted plaintext.
   Returns the recovered plaintext integer modulo 2^bit_len."
   if(bit_len <= 0){ return Z(0) }
   mut known = Z(oracle_fn(Z(c)))
   mut i = 1
   while(i < bit_len){
      def scale = power_mod(Z(2), Z(i), Z(n))
      if(gcd(scale, Z(n)) != Z(1)){ return nil }
      def inv = inverse_mod(scale, Z(n))
      def chosen = mod(Z(c) * power_mod(inv, Z(e), Z(n)), Z(n))
      def out_bit = Z(oracle_fn(chosen))
      def known_contrib = mod(known * inv, Z(n))
      def bit_i = mod(out_bit - known_contrib, Z(2))
      known = known + bit_i * (Z(1) << Z(i))
      i += 1
   }
   known
}
