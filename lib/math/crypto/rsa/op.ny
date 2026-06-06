;; Keywords: rsa op math crypto
;; RSA operand utilities routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.op(rsa_keygen, rsa_gen_keypair, rsa_encrypt, rsa_decrypt, rsa_sign, rsa_verify, compute_phi, compute_d, compute_lambda_factors, rsa_private_exponent_from_factors, rsa_decrypt_with_factors, rsa_decrypt_text_with_factors, is_perfect_square, rsa_decrypt_exponent_chain, rsa_combine_exponent_chain)
use std.math.nt
use std.math.bin

fn compute_phi(any p, any q) bigint {
   "Compute Euler's totient phi(n) = (p-1)*(q-1) for RSA primes p and q."
   (p - 1) * (q - 1)
}

fn compute_d(any e, any phi_n) bigint {
   "Compute RSA private exponent d = e^-1 mod phi(n). Returns d or 0 if not invertible."
   inverse_mod(e, phi_n)
}

fn compute_lambda_factors(list factors) any {
   "Compute Carmichael lambda for a list of distinct RSA prime factors."
   mut lam = Z(1)
   mut i = 0
   while(i < factors.len){
      lam = lcm(lam, Z(factors[i]) - Z(1))
      i += 1
   }
   lam
}

fn rsa_private_exponent_from_factors(any e, list factors) bigint {
   "Compute the RSA private exponent from e and prime factors of n."
   inverse_mod(Z(e), compute_lambda_factors(factors))
}

fn rsa_decrypt_with_factors(any c, any e, any n, list factors) ?bigint {
   "Decrypt RSA ciphertext c using public exponent e, modulus n, and the prime factors of n."
   mut prod = Z(1)
   mut i = 0
   while(i < factors.len){
      prod *= Z(factors[i])
      i += 1
   }
   if(prod != Z(n)){ return nil }
   def d = rsa_private_exponent_from_factors(e, factors)
   if(d == nil || d == 0){ return nil }
   rsa_decrypt(Z(c), d, Z(n))
}

fn rsa_decrypt_text_with_factors(any c, any e, any n, list factors) str {
   "Decrypt RSA ciphertext c with known factors and decode the plaintext integer as text."
   def m = rsa_decrypt_with_factors(c, e, n, factors)
   if(m == nil){ return "" }
   Z(m).bytes.text
}

fn is_perfect_square(any n) bool {
   "Return true if n is a perfect square."
   if(n < 0){ return false }
   def s = isqrt(n)
   s * s == n
}

fn rsa_keygen(int bits) list {
   "Generate an RSA keypair with the given modulus bit size(min 16).
   Returns [n, e, d, p, q]."
   mut p, q = random_prime(bits / 2), random_prime(bits / 2)
   while(p == q){ q = random_prime(bits / 2) }
   def n = p * q
   def phi_n = compute_phi(p, q)
   def e = 65537
   def d = compute_d(e, phi_n)
   [n, e, d, p, q]
}

fn rsa_gen_keypair(any p, any q, any e) any {
   "Construct an RSA keypair from given primes p, q and public exponent e.
   Returns [n, e, d] or nil if e is not coprime to phi(n)."
   def n = p * q
   def phi_n = compute_phi(p, q)
   if(gcd(e, phi_n) != 1){ return nil }
   def d = compute_d(e, phi_n)
   [n, e, d]
}

fn rsa_encrypt(any m, any e, any n) bigint {
   "RSA textbook encryption: c = m^e mod n. m must be < n."
   power_mod(m, e, n)
}

fn rsa_decrypt(any c, any d, any n) bigint {
   "RSA textbook decryption: m = c^d mod n."
   power_mod(c, d, n)
}

fn rsa_sign(any m, any d, any n) bigint {
   "RSA textbook signature(sign with private key): s = m^d mod n."
   power_mod(m, d, n)
}

fn rsa_verify(any s, any e, any n, any expected_m) bool {
   "RSA signature verification: check that s^e mod n == expected_m."
   power_mod(s, e, n) == expected_m
}

fn rsa_combine_exponent_chain(list exponents, any phi_n) any {
   "Combine sequential RSA public exponents into one exponent modulo phi(n)."
   mut out = Z(1)
   mut i = 0
   while(i < exponents.len){
      out = mod(out * Z(exponents[i]), phi_n)
      i += 1
   }
   out
}

fn rsa_decrypt_exponent_chain(any c, list exponents, any n, any phi_n) any {
   "Decrypt a ciphertext produced by repeatedly applying RSA with the same n
   and each exponent in exponents."
   def e_all, d_all = rsa_combine_exponent_chain(exponents, phi_n), inverse_mod(e_all, phi_n)
   if(d_all == nil || d_all == 0){ return nil }
   power_mod(c, d_all, n)
}
