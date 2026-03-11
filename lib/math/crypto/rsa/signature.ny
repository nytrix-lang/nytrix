;; Keywords: rsa signature
;; RSA signature verification and forgery analysis routines.
;; Reference:
;; - https://people.csail.mit.edu/rivest/Rsapaper.pdf
;; - https://crypto.stanford.edu/~dabo/pubs/papers/RSA-survey.pdf
module std.math.crypto.rsa.signature(rsa_raw_sign, rsa_raw_verify, rsa_raw_multiplicative_forge)
use std.math.nt
use std.math.crypto.rsa.op (compute_phi, compute_d)

fn rsa_raw_sign(number: m, number: d, number: n): number {
   "Sign integer representative m with raw RSA: s = m^d mod n."
   power_mod(mod(m, n), d, n)
}

fn rsa_raw_verify(number: m, number: s, number: e, number: n): bool {
   "Verify raw RSA signature by checking s^e = m(mod n)."
   power_mod(mod(s, n), e, n) == mod(m, n)
}

fn rsa_raw_multiplicative_forge(number: m1, number: s1, number: m2, number: s2, number: n): list {
   "Given two valid raw RSA signatures, forge one for the product message.
   Returns [m3, s3] with m3 = m1*m2 mod n and s3 = s1*s2 mod n."
   def m3, s3 = mod(m1 * m2, n), mod(s1 * s2, n)
   [m3, s3]
}
