;; Keywords: rsa manger math crypto
;; RSA Manger RSA padding-oracle attack routines.
;; Reference:
;; - Manger J., "A Chosen Ciphertext Attack on RSA Optimal Asymmetric Encryption Padding (OAEP) as Standardized in PKCS #1 v2.0"
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.manger(manger_attack)
use std.math.nt
use std.math.crypto.rsa.op (compute_phi, compute_d)

fn _floor_div(any a, any b) any { a / b }

fn _ceil_div(any a, any b) any { a / b + ((a % b) != 0 ? 1 : 0) }

fn _manger_step1(any padding_oracle, any n, any e, any c) any {
   mut f1 = 2
   while padding_oracle(mod(power_mod(f1, e, n) * c, n)) { f1 *= 2 }
   f1
}

fn _manger_step2(any padding_oracle, any n, any e, any c, any B, any f1) any {
   mut f2 = _floor_div(n + B, B) * f1 / 2
   while !padding_oracle(mod(power_mod(f2, e, n) * c, n)) { f2 += f1 / 2 }
   f2
}

fn _manger_step3(any padding_oracle, any n, any e, any c, any B, any f2) any {
   mut mmin = _ceil_div(n, f2)
   mut mmax = _floor_div(n + B, f2)
   while mmin < mmax {
      def f, i = _floor_div(2 * B, mmax - mmin), _floor_div(f * mmin, n)
      def f3 = _ceil_div(i * n, mmin)
      if padding_oracle(mod(power_mod(f3, e, n) * c, n)) { mmax = _floor_div(i * n + B, f3) } else { mmin = _ceil_div(i * n + B, f3) }
   }
   mmin
}

fn manger_attack(any padding_oracle, any n, any e, any c) any {
   "Recover plaintext with an OAEP-style oracle returning true when decrypted plaintext < B."
   def k, B = _ceil_div(bit_length(n), 8), 1 << (8 * (k - 1))
   assert(2 * B < n, "modulus too small for Manger attack interval")
   def f1, f2 = _manger_step1(padding_oracle, n, e, c), _manger_step2(padding_oracle, n, e, c, B, f1)
   _manger_step3(padding_oracle, n, e, c, B, f2)
}
