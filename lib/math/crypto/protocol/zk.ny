;; Keywords: protocol zk
;; Protocol-analysis routines for zero-knowledge transcript and nonce-reuse extraction.
;; Reference:
;; - https://datatracker.ietf.org/doc/draft-irtf-cfrg-sigma-protocols/
module std.math.crypto.protocol.zk(sigma_linear_witness_from_reuse, schnorr_secret_from_nonce_reuse, oss_public_h, oss_sign, oss_verify, fiat_shamir_challenge, zk_transcript_reused_commitment, girault_malicious_challenge_extract)
use std.math.nt
use std.math.crypto.hash
use std.core.str

fn zk_transcript_reused_commitment(list: t1, list: t2): bool {
   "Return true if two transcript tuples reuse the same commitment at index 0."
   t1.get(0) == t2.get(0)
}

fn sigma_linear_witness_from_reuse(any: z1, any: c1, any: z2, any: c2, any: q): any {
   "Extract witness w from linear Sigma responses z = r + c*w mod q."
   def den = mod(Z(c1) - Z(c2), q)
   def inv = den == Z(0) ? nil : inverse_mod(den, q)
   (inv == nil || inv == Z(0)) ? nil : mod((Z(z1) - Z(z2)) * inv, q)
}

fn girault_malicious_challenge_extract(any: z, any: e, any: r_bound): any {
   "Extract the witness from a Girault-style response z = r + e*w when
   a malicious verifier chooses e larger than the prover's randomizer bound."
   def eZ = Z(e)
   if(eZ == Z(0)){ return nil }
   def zZ = Z(z)
   def w = zZ / eZ
   def r = zZ - w * eZ
   (r >= Z(0) && r < Z(r_bound)) ? w : nil
}

fn schnorr_secret_from_nonce_reuse(list: sig1, list: sig2, any: q): any {
   "Extract Schnorr secret from two signatures/transcripts [R, c, s] sharing R."
   if(!zk_transcript_reused_commitment(sig1, sig2)){ return nil }
   sigma_linear_witness_from_reuse(sig1.get(2), sig1.get(1), sig2.get(2), sig2.get(1), q)
}

fn oss_public_h(any: n, any: k): any {
   "Compute Ong-Schnorr-Shamir public coefficient h = -(k^{-1})^2 mod n."
   def nZ, kZ = Z(n), mod(Z(k), nZ)
   if(kZ == Z(0)){ return nil }
   def kinv = inverse_mod(kZ, nZ)
   if(kinv == Z(0)){ return nil }
   mod(Z(0) - (kinv * kinv), nZ)
}

fn _oss_pick_r(any: n): any {
   mut r = Z(2)
   while(r < n){
      if(gcd(r, n) == Z(1)){ return r }
      r = r + Z(1)
   }
   nil
}

fn oss_sign(any: m, any: n, any: k, any: r=nil): any {
   "Sign message representative m with Ong-Schnorr-Shamir.
   Returns [s1, s2, h] or nil.
   If r is nil, picks the first invertible r >= 2."
   def nZ = Z(n)
   if(nZ <= Z(2)){ return nil }
   def kZ = mod(Z(k), nZ)
   def h = oss_public_h(nZ, kZ)
   if(h == nil){ return nil }
   def two_inv = inverse_mod(Z(2), nZ)
   if(two_inv == Z(0)){ return nil }
   mut rZ = r == nil ? _oss_pick_r(nZ) : mod(Z(r), nZ)
   if(rZ == nil || rZ == Z(0)){ return nil }
   if(gcd(rZ, nZ) != Z(1)){ return nil }
   def r_inv = inverse_mod(rZ, nZ)
   if(r_inv == Z(0)){ return nil }
   def mZ, mr = mod(Z(m), nZ), mod(mZ * r_inv, nZ)
   def s1, s2 = mod(two_inv * mod(mr + rZ, nZ), nZ), mod(two_inv * kZ * mod(mr - rZ, nZ), nZ)
   [s1, s2, h]
}

fn oss_verify(any: m, any: sig, any: n, any: h): bool {
   "Verify Ong-Schnorr-Shamir signature tuple [s1, s2]."
   if(!is_list(sig) || sig.len < 2){ return false }
   def nZ = Z(n)
   if(nZ <= Z(2)){ return false }
   def mZ = mod(Z(m), nZ)
   def s1 = mod(Z(sig.get(0)), nZ)
   def s2 = mod(Z(sig.get(1)), nZ)
   def hZ = mod(Z(h), nZ)
   mod(s1 * s1 + hZ * s2 * s2, nZ) == mZ
}

fn fiat_shamir_challenge(list: parts, any: q): any {
   "Hash transcript parts to a challenge modulo q using SHA-256 over their string forms."
   mut msg = Builder(parts.len * 16 + 8)
   mut i = 0
   while(i < parts.len){
      if(i > 0){ msg = builder_append(msg, "|") }
      msg = builder_append(msg, to_str(parts.get(i)))
      i += 1
   }
   def msg_s = builder_to_str(msg)
   builder_free(msg)
   def digest = sha256(msg_s)
   mut acc = Z(0)
   i = 0
   while(i < digest.len){
      acc = (acc << Z(8)) + Z(digest.get(i))
      i += 1
   }
   mod(acc, q)
}
