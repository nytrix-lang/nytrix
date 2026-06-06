;; Keywords: rsa cca-malleability math crypto
;; RSA CCA malleability demonstrations routines.
;; Demonstrates why raw RSA must never be used without padding (OAEP).
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.cca_malleability(rsa_malleable_multiply, rsa_blind_decrypt, rsa_cca_attack)
use std.core
use std.math.nt

fn rsa_malleable_multiply(any c, any s, any e, any n) any {
   "Multiply ciphertext c by s^e mod n to create a blinded ciphertext.
   The result is(c * s^e) mod n, which decrypts to(m * s) mod n.
   c: original ciphertext, s: random blinding factor, e: public exponent, n: modulus."
   def s_e = power_mod(s, e, n)
   def result = (c * s_e) % n
   result
}

fn rsa_blind_decrypt(any cipher, any e, any n, fnptr oracle_fn) any {
   "Blind the ciphertext with a random factor s, query the decryption oracle,
   and unblind the result to recover the plaintext.
   cipher: ciphertext to decrypt, e: public exponent, n: modulus,
   oracle_fn: function that decrypts a ciphertext(the decryption oracle).
   Returns the recovered plaintext."
   mut s = 2
   mut done = false
   while(!done){
      def gcd_val = gcd(s, n)
      if(gcd_val == 1){ done = true } else { s += 1 }
   }
   def blinded = rsa_malleable_multiply(cipher, s, e, n)
   def blinded_plain = oracle_fn(blinded)
   def s_inv = s.invmod(n)
   def plain = (blinded_plain * s_inv) % n
   plain
}

fn rsa_cca_attack(any c, any e, any n, fnptr oracle_fn) any {
   "Perform a full chosen-ciphertext attack to recover the plaintext.
   This attack exploits the malleability of raw RSA.
   c: target ciphertext, e: public exponent, n: modulus,
   oracle_fn: decryption oracle function that returns plaintext for any ciphertext.
   Returns the recovered plaintext m."
   def m = rsa_blind_decrypt(c, e, n, oracle_fn)
   m
}
