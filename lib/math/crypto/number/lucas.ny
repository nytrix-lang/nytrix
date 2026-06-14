;; Keywords: number-theory lucas math crypto
;; Crypto number-theory routines for Lucas sequences and related tests.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap8.pdf
;; - https://www.cryptopp.com/wiki/LUC_Cryptography
;; References:
;; - std.math.crypto.number
;; - std.math.crypto
module std.math.crypto.number.lucas(lucas_uv_mod, lucas_u_mod, lucas_v_mod, lucas_encrypt, lucas_apply_exponent_chain, lucas_private_modulus_from_factors, lucas_combine_exponent_chain, lucas_decrypt, lucas_decrypt_exponent_chain)
use std.math.nt

fn _z(any x) bigint { is_bigint(x) ? x : Z(x) }

fn _mod_n(any x, any n) bigint {
   def nz = _z(n)
   mut r = mod(_z(x), nz)
   if r < 0 { r = r + nz }
   r
}

fn _mod_z(any x, bigint n) bigint {
   mut r = _z(x) % n
   if r < Z(0) { r = r + n }
   r
}

fn _lcm(any a, any b) bigint {
   def az0, bz0 = _z(a), _z(b)
   def az, bz = az0 < 0 ? -az0 : az0, bz0 < 0 ? -bz0 : bz0
   if az == 0 || bz == 0 { return Z(0) }
   (az / gcd(az, bz)) * bz
}

fn _lucas_uv_linear(any P, any Q, any k, any n) list {
   def nz = _z(n)
   def z0, z1, z2 = Z(0), Z(1), Z(2)
   def pz, qz = _mod_z(P, nz), _mod_z(Q, nz)
   def kz = _z(k)
   if kz == z0 { return [z0, _mod_z(z2, nz)] }
   mut u0, u1 = z0, z1
   mut v0, v1 = z2 % nz, pz
   mut i = z1
   while i < kz {
      def u2, v2 = _mod_z(pz * u1 - qz * u0, nz), _mod_z(pz * v1 - qz * v0, nz)
      u0, u1 = u1, u2
      v0, v1 = v1, v2
      i = i + Z(1)
   }
   [u1, v1]
}

fn lucas_uv_mod(any P, any Q, any k, any n) list {
   "Compute [U_k(P,Q), V_k(P,Q)] mod n."
   def nz = _z(n)
   def z0, z1, z2, z4 = Z(0), Z(1), Z(2), Z(4)
   def pz, qz = _mod_z(P, nz), _mod_z(Q, nz)
   def kz = _z(k)
   if kz < z0 { panic("lucas_uv_mod: k must be non-negative") }
   if kz == z0 { return [z0, _mod_z(z2, nz)] }
   if kz == z1 { return [z1, pz] }
   if nz % z2 == z0 { return _lucas_uv_linear(pz, qz, kz, nz) }
   def inv2 = inverse_mod(z2, nz)
   if inv2 == nil || inv2 == 0 { return _lucas_uv_linear(pz, qz, kz, n) }
   def D = _mod_z(pz * pz - z4 * qz, nz)
   mut U, V = z1, pz
   mut Qk = qz
   mut b = bit_length(kz) - 2
   while b >= 0 {
      U = (U * V) % nz
      V = (V * V - z2 * Qk) % nz
      if V < z0 { V = V + nz }
      Qk = (Qk * Qk) % nz
      if ((kz >> b) & z1) != z0 {
         def Uo, Vo = ((pz * U + V) * inv2) % nz, ((D * U + pz * V) * inv2) % nz
         U, V = Uo, Vo
         Qk = (Qk * qz) % nz
      }
      b -= 1
   }
   [U, V]
}

fn lucas_u_mod(any P, any k, any n, any Q=1) bigint {
   "Compute U_k(P,Q) mod n."
   def uv = lucas_uv_mod(P, Q, k, n)
   uv[0]
}

fn lucas_v_mod(any P, any k, any n, any Q=1) bigint {
   "Compute V_k(P,Q) mod n."
   def uv = lucas_uv_mod(P, Q, k, n)
   uv[1]
}

fn lucas_encrypt(any m, any e, any n, any Q=1) bigint {
   "LUC-style encryption C = V_e(m, Q) mod n."
   lucas_v_mod(m, e, n, Q)
}

fn lucas_apply_exponent_chain(any m, list exponents, any n, any Q=1) bigint {
   "Apply multiple Lucas exponents sequentially with same modulus n."
   mut out = _mod_n(m, n)
   mut i = 0
   while i < exponents.len {
      out = lucas_v_mod(out, _z(exponents[i]), n, Q)
      i += 1
   }
   out
}

fn lucas_private_modulus_from_factors(any c, any p, any q, any Q=1) bigint {
   "Compute LUC private exponent modulus from factors p, q and ciphertext c.
   Uses lcm(p - (D/p), q - (D/q)) with D = c^2 - 4Q."
   def D = _z(c) * _z(c) - Z(4) * _z(Q)
   def jp = jacobi(D, p)
   def jq = jacobi(D, q)
   def rp = _z(p) - _z(jp)
   def rq = _z(q) - _z(jq)
   _lcm(rp, rq)
}

fn lucas_combine_exponent_chain(list exponents, any modulus) bigint {
   "Multiply public exponents modulo the private exponent modulus."
   def mz = _z(modulus)
   if mz <= 0 { return Z(0) }
   mut out = Z(1)
   mut i = 0
   while i < exponents.len {
      out = mod(out * _z(exponents[i]), mz)
      i += 1
   }
   out
}

fn lucas_decrypt(any c, any e, any p, any q, any n=nil, any Q=1) any {
   "Decrypt LUC-style ciphertext C = V_e(m,Q) mod n."
   def nz = (n == nil) ? (_z(p) * _z(q)) : _z(n)
   def ord = lucas_private_modulus_from_factors(c, p, q, Q)
   if ord <= 0 { return nil }
   def d = inverse_mod(_z(e), ord)
   if d == nil || d == 0 { return nil }
   lucas_v_mod(c, d, nz, Q)
}

fn lucas_decrypt_exponent_chain(any c, list exponents, any p, any q, any n=nil, any Q=1) any {
   "Decrypt ciphertext produced by repeated Lucas exponentiation with same n."
   def nz = (n == nil) ? (_z(p) * _z(q)) : _z(n)
   def ord = lucas_private_modulus_from_factors(c, p, q, Q)
   if ord <= 0 { return nil }
   def eall = lucas_combine_exponent_chain(exponents, ord)
   if eall == 0 { return nil }
   def d = inverse_mod(eall, ord)
   if d == nil || d == 0 { return nil }
   lucas_v_mod(c, d, nz, Q)
}
